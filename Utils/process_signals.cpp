#include "Utils/process_signals.h"

#include <csignal>

namespace
{
    volatile std::sig_atomic_t g_terminationSignal = 0;

    void handle_termination_signal(int signalNumber)
    {
        g_terminationSignal = signalNumber;
    }
}

void install_termination_signal_handlers()
{
    std::signal(SIGINT, handle_termination_signal);
    std::signal(SIGTERM, handle_termination_signal);
}

bool termination_signal_received()
{
    return g_terminationSignal != 0;
}

int termination_signal_number()
{
    return static_cast<int>(g_terminationSignal);
}

std::string termination_signal_name(int signalNumber)
{
    switch (signalNumber)
    {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    default:
        return "signal " + std::to_string(signalNumber);
    }
}
