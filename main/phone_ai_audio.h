#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fixed storage, caller-owned lifetime. No allocations in the paced media path.
class PhonePcmRing {
public:
    void Attach(int16_t* memory, size_t capacity) {
        data_ = memory;
        capacity_ = capacity;
        Clear();
    }
    void Clear() { read_ = size_ = 0; }
    size_t size() const { return size_; }
    bool Push(const int16_t* data, size_t count) {
        if (!data_ || count > capacity_ - size_)
            return false;
        for (size_t i = 0; i < count; ++i)
            data_[(read_ + size_ + i) % capacity_] = data[i];
        size_ += count;
        return true;
    }
    size_t Pop(int16_t* output, size_t count) {
        const size_t copied = std::min(count, size_);
        for (size_t i = 0; i < copied; ++i)
            output[i] = data_[(read_ + i) % capacity_];
        if (capacity_)
            read_ = (read_ + copied) % capacity_;
        size_ -= copied;
        std::fill(output + copied, output + count, 0);
        return copied;
    }

private:
    int16_t* data_ = nullptr;
    size_t capacity_ = 0, read_ = 0, size_ = 0;
};

// Streaming rational FIR: telephone 8/16 kHz <-> cloud 16/24 kHz.
// Causal history survives 20 ms packets, is reset on every media/route epoch,
// and filters before downsampling to avoid folding high frequencies into HFP.
class PhoneRateConverter {
public:
    bool Configure(unsigned source, unsigned target) {
        if (!source || !target || source % 50 || target % 50)
            return false;
        source_ = source;
        target_ = target;
        unsigned a = source, b = target;
        while (b) {
            const unsigned r = a % b;
            a = b;
            b = r;
        }
        phases_ = target / a;
        if (phases_ > 3)
            return false;
        constexpr double pi = 3.14159265358979323846;
        const double cutoff = .45 * std::min(1.0, double(target) / source);
        for (unsigned phase = 0; phase < phases_; ++phase) {
            double sum = 0;
            for (size_t j = 0; j < kTaps; ++j) {
                const double x = double(j) + double(phase) / phases_ - (kTaps - 1) / 2.;
                const double sinc =
                    std::abs(x) < 1e-10 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
                const double window = .5 - .5 * std::cos(2 * pi * j / (kTaps - 1));
                coeffs_[phase][j] = sinc * window;
                sum += coeffs_[phase][j];
            }
            for (auto& c : coeffs_[phase])
                c /= sum;
        }
        Reset();
        return true;
    }
    void Reset() { history_.fill(0); }
    bool Process(const int16_t* input, size_t count, int16_t* output, size_t capacity) {
        if (!source_ || count != source_ / 50 || capacity < target_ / 50)
            return false;
        for (size_t i = 0; i < target_ / 50; ++i) {
            const size_t numerator = i * source_;
            const size_t index = numerator / target_;
            const unsigned phase = (numerator % target_) * phases_ / target_;
            double value = 0;
            for (size_t j = 0; j < kTaps; ++j) {
                const int sample = int(index) - int(j);
                value +=
                    coeffs_[phase][j] * (sample >= 0 ? input[sample] : history_[kTaps + sample]);
            }
            output[i] = int16_t(std::clamp(std::lround(value), -32768L, 32767L));
        }
        std::copy(input + count - kTaps, input + count, history_.begin());
        return true;
    }

private:
    static constexpr size_t kTaps = 49;
    unsigned source_ = 0, target_ = 0, phases_ = 0;
    std::array<int16_t, kTaps> history_{};
    std::array<std::array<double, kTaps>, 3> coeffs_{};
};
