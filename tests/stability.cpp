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
#include <cstdlib>
#include <latch>
#include <thread>
#include <vector>
#include <chrono>

#include <unistd.h>
#include <sched.h>

using namespace std::chrono_literals;

constexpr std::uint32_t MAX_DATA_SIZE = 24;
constexpr std::uint32_t CAPACITY = 1000;
BRingBuffer<CAPACITY, MAX_DATA_SIZE> buffer;

thread_local std::uint64_t seed;
static std::uint64_t splitMix64()
{
    std::uint64_t z = (seed += 0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    z = z ^ (z >> 31);
    seed = z;
    return z;
}

static void generateData(char* const bufferPtr, const std::uint32_t dataSize)
{
    char checkSum = 0;
    for (std::uint32_t i = 0; i < dataSize - 1; ++i)
    {
        bufferPtr[i] = splitMix64() & 0xFF;
        checkSum ^= bufferPtr[i];
    }
    bufferPtr[dataSize - 1] = checkSum;
}

static bool verify(const char* const bufferPtr, const std::uint32_t dataSize)
{
    char checkSum = 0;
    for (std::uint32_t i = 0; i < dataSize - 1; ++i)
    {
        checkSum ^= bufferPtr[i];
    }

    return checkSum == bufferPtr[dataSize - 1];
}

static void setThreadAffinity(const std::uint32_t cpuId)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpuId, &set);
    if (0 != sched_setaffinity(0, sizeof(cpu_set_t), &set))
    {
       std::cout << "failed to set affinity\n";
       std::abort();
    }
}

const std::uint32_t MAX_CORES = std::thread::hardware_concurrency();
std::latch startSync{ MAX_CORES };

std::atomic<std::uint64_t> producedCounter{ 0 };
volatile bool stopProducer = false;
static void producerThread(const std::uint32_t cpuId)
{
    seed = gettid();
    setThreadAffinity(cpuId);
    std::uint64_t produced = 0;

    startSync.arrive_and_wait();

    while (!stopProducer)
    {
        void* data = buffer.reserve(MAX_DATA_SIZE);
        if (data)
        {
            generateData(static_cast<char*>(data), MAX_DATA_SIZE);
            buffer.commit(data);
            ++produced;
        }
    }
    producedCounter.fetch_add(produced);
}

std::uint64_t consumedCounter = 0;
volatile bool stopConsumer = false;
static void consumerThread(const std::uint32_t cpuId)
{
    setThreadAffinity(cpuId);
    std::uint64_t id = 0;
    startSync.arrive_and_wait();

    while (!stopConsumer)
    {
        std::uint32_t size = 0;
        void* data = buffer.peek(size, id);
        if (data)
        {
            if (false == verify(static_cast<char*>(data), size))
            {
                std::cout << "data corrupted!\n";
                std::abort();
            }
            buffer.decommit(data, id);
            ++consumedCounter;
        }
    }
}

int main()
{
    std::cout << "buffer size : " << sizeof(buffer) << " bytes\n";

    std::vector<std::thread> threads;
    threads.emplace_back(consumerThread, 0);

    for (std::uint32_t i = 1; i < MAX_CORES; ++i)
    {
        threads.emplace_back(producerThread, i);
    }

    startSync.wait();

    std::cout << "starting test...\n";
    std::this_thread::sleep_for(5min);

    stopProducer = true;
    for (std::uint32_t i = 1; i < MAX_CORES; ++i)
    {
        threads[i].join();
    }
    std::cout << "done\n";

    std::this_thread::sleep_for(100ms);
    stopConsumer = true;
    threads[0].join();

    if (consumedCounter != producedCounter)
    {
        std::cout << "test failed!\n";
    }
    std::cout << "consumed : " << consumedCounter << ", produced : " << producedCounter << "\n";

    return 0;
}
