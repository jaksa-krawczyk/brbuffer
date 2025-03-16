/*
 * Copyright 2025 Jakub Krawczyk jaksa.krawczyk at gmail com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and /or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "BRingBuffer.hpp"
#include <iostream>
#include <latch>
#include <thread>
#include <vector>
#include <chrono>

#include <unistd.h>
#include <sched.h>
#include <string.h>

using namespace std::chrono_literals;

constexpr std::uint32_t MAX_DATA_SIZE = sizeof(std::uint32_t);
constexpr std::uint32_t CAPACITY = 300;
constexpr std::uint32_t MAX_BACKOFF = 32;

static void setThreadAffinity(const std::uint32_t cpuId, const int niceness)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpuId, &set);
    if (0 != sched_setaffinity(0, sizeof(cpu_set_t), &set))
    {
       std::cout << "failed to set affinity: " << strerror(errno) << ", cpuId: " << cpuId << "\n";
       std::abort();
    }

    if (-1 == nice(niceness))
    {
        std::cout << "nice() failed: " << strerror(errno) << ", cpuId: " << cpuId << "\n";
        std::abort();
    }
}

volatile bool stopProducer = false;
static void producerThread(const std::uint32_t cpuId, std::latch& startSync, BRingBuffer<CAPACITY, MAX_DATA_SIZE>* buffer)
{
    setThreadAffinity(cpuId, -20);
    auto tid = gettid();
    std::uint32_t backoffCount = 1;
    startSync.arrive_and_wait();
    while (!stopProducer)
    {
        void* data = buffer->reserve(MAX_DATA_SIZE);
        if (data)
        {
            *static_cast<std::uint32_t*>(data) = tid;
            buffer->commit(data);
            backoffCount = 1;
        }
        else
        {
            while (backoffCount--)
            {
                asm volatile("pause");
            }
            backoffCount = backoffCount < MAX_BACKOFF ? backoffCount << 1 : MAX_BACKOFF;
        }
    }
}

std::uint64_t consumedCounter = 0;
volatile bool stopConsumer = false;
static void consumerThread(const std::uint32_t cpuId, std::latch& startSync, BRingBuffer<CAPACITY, MAX_DATA_SIZE>* buffer)
{
    setThreadAffinity(cpuId, -20);
    std::uint64_t id = 0;
    std::uint32_t backoffCount = MAX_BACKOFF;

    startSync.arrive_and_wait();
    std::uint32_t size;
    while (!stopConsumer)
    {
        size = 0;
        void* data = buffer->peek(size, id);
        if (data)
        {
            buffer->decommit(data, id);
            ++consumedCounter;
        }
        else
        {
            while (backoffCount--)
            {
                asm volatile("pause");
            }
            backoffCount = MAX_BACKOFF;
        }
    }
}

std::uint64_t testThroughput(const std::uint32_t producersCount)
{
    BRingBuffer<CAPACITY, MAX_DATA_SIZE>* buffer = new BRingBuffer<CAPACITY, MAX_DATA_SIZE>();
    stopProducer = false;
    stopConsumer = false;
    std::latch startSync{ producersCount + 2 };

    std::vector<std::thread> threads;
    std::uint32_t cpuId = 0;
    threads.emplace_back(consumerThread, cpuId++, std::ref(startSync), buffer);
    for (std::uint32_t i = 0; i < producersCount; ++i)
    {
        threads.emplace_back(producerThread, cpuId++, std::ref(startSync), buffer);
    }

    startSync.arrive_and_wait();
    std::this_thread::sleep_for(1s);

    stopProducer = true;
    for (std::uint32_t i = 1; i <= producersCount; ++i)
    {
        threads[i].join();
    }
    stopConsumer = true;
    threads[0].join();

    std::uint64_t tmp = consumedCounter;
    consumedCounter = 0;
    delete buffer;
    return tmp;
}

int main()
{
    std::cout << "buffer size: " << sizeof(BRingBuffer<CAPACITY, MAX_DATA_SIZE>) << " bytes, " << CAPACITY << " buckets\n";

    for (std::uint32_t i = 1; i < std::thread::hardware_concurrency(); ++i)
    {
        auto count = testThroughput(i);
        std::cout << i << " producers: " << count << " per second\n";
    }

    return 0;
}
