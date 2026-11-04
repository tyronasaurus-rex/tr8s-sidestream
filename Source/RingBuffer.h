#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

/** Lock-free single-producer / single-consumer planar float ring buffer. */
class PlanarRingBuffer
{
public:
    void resize (int numChannels, int capacitySamples)
    {
        jassert (numChannels > 0);
        jassert (capacitySamples > 1);

        channels = numChannels;
        capacity = capacitySamples;
        storage.assign ((size_t) channels * (size_t) capacity, 0.0f);
        writeIndex.store (0, std::memory_order_relaxed);
        readIndex.store (0, std::memory_order_relaxed);
        underruns.store (0, std::memory_order_relaxed);
        overruns.store (0, std::memory_order_relaxed);
        slips.store (0, std::memory_order_relaxed);
    }

    int getNumChannels() const noexcept { return channels; }
    int getCapacity() const noexcept { return capacity; }

    int getAvailableToRead() const noexcept
    {
        const int w = writeIndex.load (std::memory_order_acquire);
        const int r = readIndex.load (std::memory_order_relaxed);
        return (w - r + capacity) % capacity;
    }

    int getAvailableToWrite() const noexcept
    {
        return capacity - 1 - getAvailableToRead();
    }

    int discard (int numSamples) noexcept
    {
        if (numSamples <= 0 || capacity <= 0)
            return 0;

        const int avail = getAvailableToRead();
        const int n = juce::jmin (numSamples, avail);
        if (n <= 0)
            return 0;

        const int r = readIndex.load (std::memory_order_relaxed);
        readIndex.store ((r + n) % capacity, std::memory_order_release);
        slips.fetch_add (1, std::memory_order_relaxed);
        return n;
    }

    int write (const float* const* src, int numSamples) noexcept
    {
        if (channels <= 0 || capacity <= 0 || src == nullptr || numSamples <= 0)
            return 0;

        int written = 0;
        while (written < numSamples)
        {
            int space = getAvailableToWrite();
            if (space <= 0)
            {
                const int drop = juce::jmax (1, capacity / 4);
                discard (drop);
                overruns.fetch_add (1, std::memory_order_relaxed);
                space = getAvailableToWrite();
                if (space <= 0)
                    break;
            }

            const int w = writeIndex.load (std::memory_order_relaxed);
            const int chunk = juce::jmin (numSamples - written, space, capacity - w);

            for (int ch = 0; ch < channels; ++ch)
            {
                if (src[ch] != nullptr)
                    juce::FloatVectorOperations::copy (storage.data() + (size_t) ch * (size_t) capacity + (size_t) w,
                                                       src[ch] + written,
                                                       chunk);
                else
                    juce::FloatVectorOperations::clear (storage.data() + (size_t) ch * (size_t) capacity + (size_t) w,
                                                        chunk);
            }

            writeIndex.store ((w + chunk) % capacity, std::memory_order_release);
            written += chunk;
        }

        return written;
    }

    void read (float* const* dst, int numSamples) noexcept
    {
        if (dst == nullptr || numSamples <= 0)
            return;

        int readCount = 0;
        while (readCount < numSamples)
        {
            const int avail = getAvailableToRead();
            if (avail <= 0)
            {
                underruns.fetch_add (1, std::memory_order_relaxed);
                for (int ch = 0; ch < channels; ++ch)
                    if (dst[ch] != nullptr)
                        juce::FloatVectorOperations::clear (dst[ch] + readCount, numSamples - readCount);
                return;
            }

            const int r = readIndex.load (std::memory_order_relaxed);
            const int chunk = juce::jmin (numSamples - readCount, avail, capacity - r);

            for (int ch = 0; ch < channels; ++ch)
            {
                if (dst[ch] != nullptr)
                    juce::FloatVectorOperations::copy (dst[ch] + readCount,
                                                       storage.data() + (size_t) ch * (size_t) capacity + (size_t) r,
                                                       chunk);
            }

            readIndex.store ((r + chunk) % capacity, std::memory_order_release);
            readCount += chunk;
        }
    }

    /**
     * Linear-interpolation SRC read into numOut host samples.
     * ratio = capture samples consumed per host sample (1.0 = locked).
     * fracPhase holds [0,1) residual between calls.
     * Needs ~numOut*ratio+2 samples available; otherwise soft-underruns.
     */
    void readResampled (float* const* dst, int numOut, double ratio, double& fracPhase) noexcept
    {
        if (dst == nullptr || numOut <= 0 || channels <= 0 || capacity <= 0)
            return;

        ratio = juce::jlimit (0.90, 1.10, ratio);
        if (fracPhase < 0.0 || fracPhase >= 1.0)
            fracPhase = 0.0;

        const int avail = getAvailableToRead();
        const double need = (double) numOut * ratio + 2.0;
        if (avail < (int) std::ceil (need))
        {
            underruns.fetch_add (1, std::memory_order_relaxed);
            for (int ch = 0; ch < channels; ++ch)
                if (dst[ch] != nullptr)
                    juce::FloatVectorOperations::clear (dst[ch], numOut);
            return;
        }

        const int r0 = readIndex.load (std::memory_order_relaxed);
        double pos = fracPhase; // offset from r0 in capture samples
        double maxPos = 0.0;

        for (int i = 0; i < numOut; ++i)
        {
            const int i0 = (int) pos;
            const float f = (float) (pos - (double) i0);
            const int idx0 = (r0 + i0) % capacity;
            const int idx1 = (idx0 + 1) % capacity;

            for (int ch = 0; ch < channels; ++ch)
            {
                if (dst[ch] == nullptr)
                    continue;
                const float* base = storage.data() + (size_t) ch * (size_t) capacity;
                dst[ch][i] = base[idx0] * (1.0f - f) + base[idx1] * f;
            }

            pos += ratio;
            maxPos = pos;
        }

        const int advance = (int) maxPos; // floor
        fracPhase = maxPos - (double) advance;
        readIndex.store ((r0 + advance) % capacity, std::memory_order_release);
    }

    void reset() noexcept
    {
        writeIndex.store (0, std::memory_order_relaxed);
        readIndex.store (0, std::memory_order_relaxed);
        if (! storage.empty())
            juce::FloatVectorOperations::clear (storage.data(), (int) storage.size());
    }

    int getUnderruns() const noexcept { return underruns.load (std::memory_order_relaxed); }
    int getOverruns() const noexcept { return overruns.load (std::memory_order_relaxed); }
    int getSlips() const noexcept { return slips.load (std::memory_order_relaxed); }

    void clearCounters() noexcept
    {
        underruns.store (0, std::memory_order_relaxed);
        overruns.store (0, std::memory_order_relaxed);
        slips.store (0, std::memory_order_relaxed);
    }

private:
    int channels = 0;
    int capacity = 0;
    std::vector<float> storage;
    std::atomic<int> writeIndex { 0 };
    std::atomic<int> readIndex { 0 };
    std::atomic<int> underruns { 0 };
    std::atomic<int> overruns { 0 };
    std::atomic<int> slips { 0 };
};
