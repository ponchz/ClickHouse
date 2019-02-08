#include <Processors/Executors/PipelineExecutor.h>
#include <unordered_map>
#include <queue>
#include <IO/WriteBufferFromString.h>
#include <Processors/printPipeline.h>
#include <Common/EventCounter.h>

namespace DB
{

PipelineExecutor::PipelineExecutor(Processors processors, ThreadPool * pool)
    : processors(std::move(processors)), pool(pool)
{
    buildGraph();
}


void PipelineExecutor::buildGraph()
{
    std::unordered_map<const IProcessor *, UInt64> proc_map;
    UInt64 num_processors = processors.size();

    auto throwUnknownProcessor = [](const IProcessor * proc, const IProcessor * parent, bool from_input_port)
    {
        String msg = "Processor " + proc->getName() + " was found as " + (from_input_port ? "input" : "output")
                + " for processor " + parent->getName() + ", but not found in original list or all processors.";

        throw Exception(msg, ErrorCodes::LOGICAL_ERROR);
    };

    graph.resize(num_processors);
    for (UInt64 node = 0; node < num_processors; ++node)
    {
        IProcessor * proc = processors[node].get();
        proc_map[proc] = node;
        graph[node].processor = proc;
    }

    for (UInt64 node = 0; node < num_processors; ++node)
    {
        const IProcessor * cur = graph[node].processor;

        for (InputPort & input_port : processors[node]->getInputs())
        {
            const IProcessor * proc = &input_port.getProcessor();

            auto it = proc_map.find(proc);
            if (it == proc_map.end())
                throwUnknownProcessor(proc, cur, true);

            UInt64 proc_num = it->second;
            bool new_edge = true;
            for (UInt64 edge = 0; new_edge && edge < graph[node].backEdges.size(); ++edge)
                if (graph[node].backEdges[edge].to == proc_num)
                    new_edge = false;

            if (new_edge)
            {
                graph[node].backEdges.emplace_back();
                graph[node].backEdges.back().to = proc_num;
                input_port.setVersion(&graph[node].backEdges.back().version);
            }
        }

        for (OutputPort & output_port : processors[node]->getOutputs())
        {
            const IProcessor * proc = &output_port.getProcessor();

            auto it = proc_map.find(proc);
            if (it == proc_map.end())
                throwUnknownProcessor(proc, cur, true);

            UInt64 proc_num = it->second;
            bool new_edge = true;
            for (UInt64 edge = 0; new_edge && edge < graph[node].directEdges.size(); ++edge)
                if (graph[node].directEdges[edge].to == proc_num)
                    new_edge = false;

            if (new_edge)
            {
                graph[node].directEdges.emplace_back();
                graph[node].directEdges.back().to = proc_num;
                output_port.setVersion(&graph[node].directEdges.back().version);
            }
        }
    }
}

void PipelineExecutor::addChildlessProcessorsToQueue()
{
    UInt64 num_processors = processors.size();
    for (UInt64 proc = 0; proc < num_processors; ++proc)
    {
        if (graph[proc].directEdges.empty())
        {
            prepare_queue.push(proc);
            graph[proc].status = ExecStatus::Preparing;
        }
    }
}

void PipelineExecutor::processFinishedExecutionQueue()
{
    while (!finished_execution_queue.empty())
    {
        UInt64 proc = finished_execution_queue.front();
        finished_execution_queue.pop();

        graph[proc].status = ExecStatus::Preparing;
        prepare_queue.push(proc);
    }
}

void PipelineExecutor::processFinishedExecutionQueueSafe()
{
    if (pool)
    {
        exception_handler.throwIfException();
        std::lock_guard lock(finished_execution_mutex);
        processFinishedExecutionQueue();
    }
    else
        processFinishedExecutionQueue();
}

bool PipelineExecutor::addProcessorToPrepareQueueIfCan(Edge & edge)
{
    /// Don't add processor if nothing was read from port.
    if (edge.version != edge.prev_version)
        return false;

    auto & node = graph[edge.to];
    if (node.status == ExecStatus::Idle)
    {
        prepare_queue.push(edge.to);
        node.status = ExecStatus::Preparing;
        return true;
    }

    return false;
}

void PipelineExecutor::addJob(UInt64 pid)
{
    if (pool)
    {
        auto job = [this, pid]()
        {
            graph[pid].processor->work();

            {
                std::lock_guard lock(finished_execution_mutex);
                finished_execution_queue.push(pid);
            }

            event_counter.notify();
        };

        pool->schedule(createExceptionHandledJob(std::move(job), exception_handler));
        ++num_tasks_to_wait;
    }
    else
    {
        /// Execute task in main thread.
        graph[pid].processor->work();
        finished_execution_queue.push(pid);
    }
}

void PipelineExecutor::addAsyncJob(UInt64 pid)
{
    graph[pid].processor->schedule(event_counter);
    ++num_tasks_to_wait;
}

void PipelineExecutor::prepareProcessor(UInt64 pid, bool async)
{
    auto & node = graph[pid];
    auto status = node.processor->prepare();
    node.last_processor_status = status;

    auto add_neighbours_to_prepare_queue = [&, this]
    {
        for (auto & edge : node.directEdges)
            addProcessorToPrepareQueueIfCan(edge);

        for (auto & edge : node.backEdges)
            addProcessorToPrepareQueueIfCan(edge);
    };

    switch (status)
    {
        case IProcessor::Status::NeedData:
        {
            add_neighbours_to_prepare_queue();
            node.status = ExecStatus::Idle;
            break;
        }
        case IProcessor::Status::PortFull:
        {
            add_neighbours_to_prepare_queue();
            node.status = ExecStatus::Idle;
            break;
        }
        case IProcessor::Status::Finished:
        {
            add_neighbours_to_prepare_queue();
            node.status = ExecStatus::Finished;
            break;
        }
        case IProcessor::Status::Ready:
        {
            node.status = ExecStatus::Executing;
            addJob(pid);
            break;
        }
        case IProcessor::Status::Async:
        {
            node.status = ExecStatus::Executing;
            addAsyncJob(pid);
            break;
        }
        case IProcessor::Status::Wait:
        {
            if (!async)
                throw Exception("Processor returned status Wait before Async.", ErrorCodes::LOGICAL_ERROR);
        }
    }
}

void PipelineExecutor::processPrepareQueue()
{
    while (!prepare_queue.empty())
    {
        UInt64 proc = prepare_queue.front();
        prepare_queue.pop();

        prepareProcessor(proc, false);

    }
}

void PipelineExecutor::processAsyncQueue()
{
    UInt64 num_processors = processors.size();
    for (UInt64 node = 0; node < num_processors; ++node)
        if (graph[node].status == ExecStatus::Async)
            prepareProcessor(node, true);
}

void PipelineExecutor::execute()
{
    addChildlessProcessorsToQueue();

    while (true)
    {
        processFinishedExecutionQueueSafe();
        processPrepareQueue();
        processAsyncQueue();

        if (prepare_queue.empty())
        {
            /// For single-thread executor.
            if (!pool && !finished_execution_queue.empty())
                continue;

            if (num_tasks_to_wait > num_waited_tasks)
            {
                /// Try wait anything.
                event_counter.wait();
                ++num_waited_tasks;
            }
            else
            {
                /// Here prepare_queue is empty and we have nobody to wait for. Exiting.
                break;
            }
        }
    }

    bool all_processors_finished = true;
    for (auto & node : graph)
        if (node.status != ExecStatus::Finished)
            all_processors_finished = false;

    if (!all_processors_finished)
    {
        /// It seems that pipeline has stuck.

        std::vector<IProcessor::Status> statuses;
        std::vector<IProcessor *> proc_list;
        statuses.reserve(graph.size());
        proc_list.reserve(graph.size());

        for (auto & proc : graph)
        {
            proc_list.emplace_back(proc.processor);
            statuses.emplace_back(proc.last_processor_status);
        }

        WriteBufferFromOwnString out;
        printPipeline(processors, statuses, out);
        out.finish();

        throw Exception("Pipeline stuck. Current state:\n" + out.str(), ErrorCodes::LOGICAL_ERROR);
    }
}

}
