#include <thread>

#include "src/lockfree_queue.h"

using namespace ksgw::internal;
using namespace ksgw::list_based;

struct alignas(CacheLineSize) producer_t {
    uint64_t counter;
};

struct alignas(CacheLineSize) consumer_t {
    uint64_t sum;
};

struct TaskConext {
    producer_t producer_info[8];
    consumer_t consumer_info[8];
};

size_t produce_proc(TaskConext* context, size_t id, Queue<uint64_t>& queue){
    uint64_t i = 0;
    while(i<1000000) {
        queue.push(std::move(i));
        context->producer_info[id].counter+=i;
        ++i;
    }
    return 0;
}

size_t consumer_proc(TaskConext* context, size_t id, Queue<uint64_t>& queue){
    uint64_t i = 0;
    while(i<1000000) {
        uint64_t data;
        if(queue.pop(data) == 0) {
            context->consumer_info[id].sum+=data;
            i++;
        }
    }
    return 0;
}

TaskConext taskContext = { {}, {} };

int main() {
    Queue<uint64_t> q;

    std::thread producer_threads[] = {
        std::thread(produce_proc, &taskContext, 0, std::ref(q)),
        std::thread(produce_proc, &taskContext, 1, std::ref(q)),
        std::thread(produce_proc, &taskContext, 2, std::ref(q)),
        std::thread(produce_proc, &taskContext, 3, std::ref(q)),
        std::thread(produce_proc, &taskContext, 4, std::ref(q)),
        std::thread(produce_proc, &taskContext, 5, std::ref(q)),
        std::thread(produce_proc, &taskContext, 6, std::ref(q)),
        std::thread(produce_proc, &taskContext, 7, std::ref(q))
    };
    std::thread consumer_threads[] = {
        std::thread(consumer_proc, &taskContext, 0, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 1, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 2, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 3, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 4, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 5, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 6, std::ref(q)),
        std::thread(consumer_proc, &taskContext, 7, std::ref(q))
    };

    for(auto& t : producer_threads) {
        t.join();
    }
    for(auto& t : consumer_threads) {
        t.join();
    }
	return 0;
}