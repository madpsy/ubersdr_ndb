#pragma once

#include <algorithm>
#include <vector>
#include <cmath>

#if defined(_WIN32) && !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif

struct GoertzelRunningFIR {
    void init(
            float sampleRate,
            int window_samples,
            float history_s) {
        m_sampleRate = sampleRate;
        m_hamming.resize(window_samples);
        for (int i = 0; i < window_samples; i++) {
            m_hamming[i] = 0.54 - 0.46*std::cos((2.0*M_PI*i)/window_samples);
        }

        int history_samples = history_s*sampleRate;

        m_historyHead = 0;
        m_histN = history_samples;
        m_history.assign(2*history_samples, 0.0f);
        m_tabFreq = -1.0f;

        m_filteredHead = 0;
        m_filtered.resize(history_samples - window_samples, 0);
        m_filteredOut.resize(history_samples - window_samples, 0);

        m_processed_samples = 0;
    }

    void process(float * samples, int n, float frequency_hz) {
        int nw = (int) m_hamming.size();
        int nh = m_histN;
        int nf = (int) m_filtered.size();

        float normalizedfreq = frequency_hz/m_sampleRate;

        float w = 2*M_PI*normalizedfreq;
        float wr = std::cos(w);
        float wi = std::sin(w);

        m_coeff = 2.0*wr;
        m_cos = wr;
        m_sin = wi;
        table(frequency_hz);

        for (int i = 0; i < n; ++i) {
            m_history[m_historyHead] = samples[i];
            m_history[m_historyHead + nh] = samples[i];
            m_historyHead++;
            if (m_historyHead >= nh) {
                m_historyHead = 0;
            }

            m_processed_samples++;
            if (m_processed_samples >= nw) {
                m_filtered[m_filteredHead] = filter(m_historyHead - nw);
                m_filteredHead++;
                if (m_filteredHead >= nf) {
                    m_filteredHead = 0;
                }
            }
        }
    }

    void recompute(float frequency_hz) {
        int nw = (int) m_hamming.size();
        int nh = m_histN;
        int nf = (int) m_filtered.size();

        float normalizedfreq = frequency_hz/m_sampleRate;

        float w = 2*M_PI*normalizedfreq;
        float wr = std::cos(w);
        float wi = std::sin(w);

        m_coeff = 2.0*wr;
        m_cos = wr;
        m_sin = wi;
        table(frequency_hz);

        m_processed_samples = 0;

        for (int i = 0; i < nh; ++i) {
            m_historyHead++;
            if (m_historyHead >= nh) {
                m_historyHead = 0;
            }

            m_processed_samples++;
            if (m_processed_samples >= nw) {
                m_filtered[m_filteredHead] = filter(m_historyHead - nw);
                m_filteredHead++;
                if (m_filteredHead >= nf) {
                    m_filteredHead = 0;
                }
            }
        }
    }

    const std::vector<float> & filtered() {
        // ubersdr_ndb: the ring unrolled oldest first, as two copies.
        const size_t nf = m_filtered.size(), head = size_t(m_filteredHead);
        std::copy(m_filtered.begin() + head, m_filtered.end(), m_filteredOut.begin());
        std::copy(m_filtered.begin(), m_filtered.begin() + head, m_filteredOut.begin() + (nf - head));
        return m_filteredOut;
    }

    const std::vector<float> & filtered_min(int w) {
        int nf = (int) m_filtered.size();

        int j = m_filteredHead;
        for (int i = 0; i < nf; ++i) {
            int j2 = j - std::min(i, w);                  // !!!! Need to double-check these computations
            int l = std::min(i, w) + std::min(nf - i, w); // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            if (j2 < 0) j2 += nf;
            float f = m_filtered[j2];
            for (int k = 0; k < l; ++k) {
                f = std::min(f, m_filtered[j2]);
                if (++j2 >= nf) j2 = 0;
            }
            m_filteredOut[i] = f;

            j++;
            if (j >= nf) {
                j = 0;
            }
        }

        return m_filteredOut;
    }

    void clear() {
        m_processed_samples = 0;
        std::fill(m_history.begin(), m_history.end(), 0.0f);
        std::fill(m_filtered.begin(), m_filtered.end(), 0.0f);
    }

private:
    // ubersdr_ndb: the windowed Goertzel over the n samples from idx, whose
    // result is |sum over i of w[i] x[idx + i] e^(-j w i)|^2, taken as that
    // sum directly: a dot product with a table of w[i] e^(-j w i), made once
    // per frequency, with independent partial sums the compiler vectorises.
    // Goertzel's recursion is one long dependency chain, and ran every audio
    // sample, for most of the decoder's time. The history is kept twice over
    // so the n samples never wrap.
    void table(float frequency_hz) {
        if (frequency_hz == m_tabFreq) return;
        m_tabFreq = frequency_hz;
        const int n = (int) m_hamming.size();
        m_tabRe.resize(n);
        m_tabIm.resize(n);
        const double w = 2.0*M_PI*double(frequency_hz)/double(m_sampleRate);
        for (int i = 0; i < n; ++i) {
            m_tabRe[i] = float(m_hamming[i]*std::cos(w*i));
            m_tabIm[i] = float(-m_hamming[i]*std::sin(w*i));
        }
    }

    float filter(int idx) {
        if (idx < 0) idx += m_histN;

        constexpr int K = 8;
        float re[K] = {}, im[K] = {};
        const float * x = m_history.data() + idx;
        const float * tr = m_tabRe.data();
        const float * ti = m_tabIm.data();
        const int n = (int) m_tabRe.size();
        int i = 0;
        for (; i + K <= n; i += K) {
            for (int k = 0; k < K; ++k) {
                re[k] += x[i + k]*tr[i + k];
                im[k] += x[i + k]*ti[i + k];
            }
        }
        for (; i < n; ++i) {
            re[0] += x[i]*tr[i];
            im[0] += x[i]*ti[i];
        }
        float sr = 0.0f, si = 0.0f;
        for (int k = 0; k < K; ++k) {
            sr += re[k];
            si += im[k];
        }
        return sr*sr + si*si;
    }

    int m_processed_samples = 0;

    float m_sampleRate = 0.0f;
    float m_coeff = 0.0f;
    float m_sin = 0.0f;
    float m_cos = 0.0f;

    std::vector<float> m_hamming;

    int m_historyHead = 0;
    int m_histN = 0;
    std::vector<float> m_history;   // m_histN samples, twice over

    float m_tabFreq = -1.0f;
    std::vector<float> m_tabRe, m_tabIm;

    int m_filteredHead = 0;
    std::vector<float> m_filtered;
    std::vector<float> m_filteredOut;
};
