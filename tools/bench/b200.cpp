
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread.hpp>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <vector>
#include <complex>
#include <string>

/* find: list devices.  rx <freq_hz> <rate_hz> <seconds> <gain_db> <out_file> */
int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: b200 find | rx <f> <rate> <s> <gain> <file>\n"); return 2; }
    std::string cmd = argv[1];
    try {
        if (cmd == "find") {
            uhd::device_addr_t hint;
            auto res = uhd::device::find(hint);
            std::printf("found %zu device(s)\n", res.size());
            for (auto& d : res) std::printf("  %s\n", d.to_string().c_str());
            return res.empty() ? 1 : 0;
        }
        if (cmd == "rx") {
            if (argc < 7) { std::fprintf(stderr, "rx needs f rate seconds gain file\n"); return 2; }
            double f = std::atof(argv[2]), rate = std::atof(argv[3]), secs = std::atof(argv[4]), gain = std::atof(argv[5]);
            uhd::device_addr_t args;
            uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
            usrp->set_rx_rate(rate);
            usrp->set_rx_freq(uhd::tune_request_t(f));
            usrp->set_rx_gain(gain);
            usrp->set_rx_antenna("RX2");
            uhd::stream_args_t sa("fc32", "sc16");
            uhd::rx_streamer::sptr rx = usrp->get_rx_stream(sa);
            size_t n = (size_t)(rate * secs);
            std::vector<std::complex<float>> buf(32768);
            FILE* fp = std::fopen(argv[6], "wb");
            if (!fp) { std::perror("fopen"); return 1; }
            uhd::stream_cmd_t sc(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
            sc.num_samps = n; sc.stream_now = true;
            rx->issue_stream_cmd(sc);
            uhd::rx_metadata_t md; size_t total = 0; size_t over = 0;
            while (total < n) {
                size_t got = rx->recv(&buf.front(), buf.size(), md, 1.0);
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) { over++; continue; }
                if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) { std::fprintf(stderr, "rx err %s\n", md.strerror().c_str()); break; }
                if (got == 0) continue;
                total += got;
                std::fwrite(&buf.front(), sizeof(std::complex<float>), got, fp);
            }
            std::fclose(fp);
            std::printf("captured %zu samples (%.3f s) at %.6f MHz, rate %.0f, gain %.1f dB, overflows %zu\n",
                        total, (double)total/rate, f/1e6, rate, gain, over);
            return 0;
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
    return 2;
}
