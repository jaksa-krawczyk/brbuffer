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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
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

#ifndef __BRINGBUFFER_HPP
#define __BRINGBUFFER_HPP

#include <cstddef>
#include <atomic>
#include <new>

template<std::uint32_t capacity, std::uint32_t maxDataSize>
class BRingBuffer
{
private:
    alignas (std::hardware_destructive_interference_size) std::atomic<std::uint64_t> readHead{ 0 };
    alignas (std::hardware_destructive_interference_size) std::atomic<std::uint64_t> writeHead{ 0 };

    struct alignas(std::hardware_destructive_interference_size) Bucket
    {
        std::atomic<bool> used{ false };
        std::uint32_t size = 0;
        char payload[maxDataSize] = { 0 };
    };
    Bucket data[capacity];

    static constexpr std::uint32_t OFFSET_TO_USED = offsetof(Bucket, payload);
    static constexpr std::uint64_t WRAP_COUNT_INCR = 0x0000000100000000;
    static constexpr std::uint64_t WRAP_COUNT_MASK = 0xFFFFFFFF00000000;
    static constexpr std::uint64_t INDEX_MASK = 0x00000000FFFFFFFF;

public:
    void* reserve(const std::uint32_t dataSize)
    {
        std::uint64_t currentWriteHead, currentReadHead, newWriteHead;
        std::uint32_t freeId, readId;
        currentWriteHead = writeHead.load(std::memory_order_relaxed);

        do
        {
            newWriteHead = currentWriteHead;
            currentReadHead = readHead.load(std::memory_order_acquire);

            freeId = currentWriteHead & INDEX_MASK;
            if (capacity == freeId)
            {
                newWriteHead = (currentWriteHead & WRAP_COUNT_MASK) + WRAP_COUNT_INCR;
                freeId = 0;
            }

            readId = currentReadHead & INDEX_MASK;
            if (WRAP_COUNT_INCR == (newWriteHead & WRAP_COUNT_MASK) - (currentReadHead & WRAP_COUNT_MASK) && readId == freeId)
            {
                return nullptr;
            }
            ++newWriteHead;

        } while (!writeHead.compare_exchange_strong(currentWriteHead, newWriteHead, std::memory_order_relaxed, std::memory_order_relaxed));

        data[freeId].size = dataSize;
        return &data[freeId].payload;
    }

    void commit(void* const dataPtr)
    {
        reinterpret_cast<Bucket*>((static_cast<char*>(dataPtr) - OFFSET_TO_USED))->used.store(true, std::memory_order_release);
    }

    void* peek(std::uint32_t& dataSize, const std::uint64_t magicId)
    {
        const std::uint32_t readId = magicId & INDEX_MASK;

        if (!data[readId].used.load(std::memory_order_acquire))
        {
            return nullptr;
        }

        dataSize = data[readId].size;
        return &data[readId].payload;
    }

    void decommit(void* const dataPtr, std::uint64_t& magicId)
    {
        const std::uint32_t readId = magicId & INDEX_MASK;
        std::uint64_t newRead = magicId + 1;

        if (capacity == readId + 1)
        {
            newRead = (magicId & WRAP_COUNT_MASK) + WRAP_COUNT_INCR;
        }
        magicId = newRead;
        reinterpret_cast<Bucket*>((static_cast<char*>(dataPtr) - OFFSET_TO_USED))->used.store(false, std::memory_order_relaxed);
        readHead.store(newRead, std::memory_order_release);
    }
};

#endif
