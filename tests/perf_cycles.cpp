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
#include <fstream>

#include <unistd.h>
#include <sched.h>
#include <string.h>

#include <sys/mman.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

constexpr std::uint32_t MAX_DATA_SIZE = sizeof(std::uint32_t);
constexpr std::uint32_t CAPACITY = 300;
constexpr std::uint32_t MAX_ELEMENTS = 5000;
constexpr std::uint32_t MAX_BACKOFF = 32;
BRingBuffer<CAPACITY, MAX_DATA_SIZE> buffer;

std::vector<std::uint64_t> producerCycles;
std::vector<std::uint64_t> consumerCycles;

static void setThreadAffinity(const std::uint32_t cpuId)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpuId, &set);
    if (0 != sched_setaffinity(0, sizeof(cpu_set_t), &set))
    {
       std::cout << "failed to set affinity, cpuId: " << cpuId << "\n";
       std::abort();
    }

    if (-1 == nice(-20))
    {
        std::cout << "nice() failed: " << strerror(errno) << "\n";
        std::abort();
    }
}

static std::uint64_t rdpmc(const std::uint32_t counter)
{
    std::uint32_t low, high;
    asm volatile("rdpmc" : "=a" (low), "=d" (high) : "c" (counter));

    return low | static_cast<std::uint64_t>(high) << 32;
}

static std::uint64_t getCycles(perf_event_mmap_page* ptr)
{
    std::uint32_t seqLock, idx;
    std::uint64_t count;

    do
    {
        seqLock = ptr->lock;
        asm volatile("" ::: "memory");

        idx = ptr->index;
        count = ptr->offset;
        if (idx)
        {
            count += rdpmc(idx - 1);
        }

        asm volatile("" ::: "memory");
    } while (ptr->lock != seqLock);
    return count;
}

struct EventData
{
    perf_event_mmap_page* ptr = nullptr;
    int fd = 0;
};

static EventData getEventPage(const std::uint32_t cpuId)
{
    perf_event_attr attr = {
        .type = PERF_TYPE_HARDWARE,
        .config = PERF_COUNT_HW_CPU_CYCLES,
        .exclude_kernel = 1
    };

    int fd = syscall(SYS_perf_event_open, &attr, gettid(), cpuId, -1, 0);
    if (fd == -1)
    {
        std::cout << "syscall() failed: " << strerror(errno) << "\n";
        std::abort();
    }

    void* ptr = mmap(NULL, getpagesize(), PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
    {
        std::cout << "mmap() failed: " << strerror(errno) << "\n";
        std::abort();
    }

    EventData eventData = { .ptr = static_cast<perf_event_mmap_page*>(ptr), .fd = fd };
    return eventData;
}

std::latch startSync{ 2 };

static void producerThread(const std::uint32_t cpuId)
{
    setThreadAffinity(cpuId);
    EventData eventData = getEventPage(cpuId);
    auto tid = gettid();
    std::uint32_t backoffCount = 1;
    ioctl(eventData.fd, PERF_EVENT_IOC_ENABLE, 0);

    startSync.arrive_and_wait();
    for (std::uint32_t i = 0; i < MAX_ELEMENTS;)
    {
        ioctl(eventData.fd, PERF_EVENT_IOC_RESET, 0);

        auto beg = getCycles(eventData.ptr);
        void* data = buffer.reserve(MAX_DATA_SIZE);
        if (data)
        {
            *static_cast<std::uint32_t*>(data) = tid + i;
            buffer.commit(data);
            auto end = getCycles(eventData.ptr);
            producerCycles.push_back(end - beg);
            ++i;
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
    ioctl(eventData.fd, PERF_EVENT_IOC_DISABLE, 0);
    munmap(eventData.ptr, getpagesize());
}

static void consumerThread(const std::uint32_t cpuId)
{
    setThreadAffinity(cpuId);
    EventData eventData = getEventPage(cpuId);
    std::uint64_t id = 0;
    std::uint32_t backoffCount = MAX_BACKOFF;
    volatile std::uint32_t tmp = 0;
    ioctl(eventData.fd, PERF_EVENT_IOC_ENABLE, 0);

    startSync.arrive_and_wait();
    std::uint32_t size;
    for (std::uint32_t i = 0; i < MAX_ELEMENTS;)
    {
        ioctl(eventData.fd, PERF_EVENT_IOC_RESET, 0);
        size = 0;

        auto beg = getCycles(eventData.ptr);
        void* data = buffer.peek(size, id);
        if (data)
        {
            tmp += *static_cast<std::uint32_t*>(data);
            buffer.decommit(data, id);
            auto end = getCycles(eventData.ptr);
            consumerCycles.push_back(end - beg);
            ++i;
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
    ioctl(eventData.fd, PERF_EVENT_IOC_DISABLE, 0);
    munmap(eventData.ptr, getpagesize());
}

void rdpmcTest()
{
    const std::uint32_t cpuId = 4;
    setThreadAffinity(cpuId);

    EventData eventData = getEventPage(cpuId);
    ioctl(eventData.fd, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(eventData.fd, PERF_EVENT_IOC_RESET, 0);

    std::vector<std::uint64_t> vec(MAX_ELEMENTS, 0);
    for (auto& var : vec)
    {
        var = getCycles(eventData.ptr);
    }

    ioctl(eventData.fd, PERF_EVENT_IOC_DISABLE, 0);
    munmap(eventData.ptr, getpagesize());

    std::ofstream rdpmcOutput("rdpmc.csv");
    rdpmcOutput << "iteration;cycles\n";
    for (std::uint32_t i = 0; i < MAX_ELEMENTS - 1; ++i)
    {
        rdpmcOutput << i + 1 << ";" << vec[i + 1] - vec[i] << "\n";
    }
    rdpmcOutput.close();
}

int main()
{
    std::cout << "buffer size : " << sizeof(buffer) << " bytes\n";

    producerCycles.reserve(MAX_ELEMENTS);
    consumerCycles.reserve(MAX_ELEMENTS);

    rdpmcTest();

    std::vector<std::thread> threads;
    threads.emplace_back(consumerThread, 0);
    threads.emplace_back(producerThread, 1);

    threads[1].join();
    threads[0].join();

    std::ofstream outputCsv("cpu_cycles.csv");
    outputCsv << "iteration;producerCycles;consumerCycle\n";
    for (std::uint32_t i = 0; i < MAX_ELEMENTS; ++i)
    {
        outputCsv << i + 1 << ";" << producerCycles[i] << ";" << consumerCycles[i] << "\n";
    }
    outputCsv.close();

    return 0;
}
