#pragma once

#include <string>

void install_termination_signal_handlers();
bool termination_signal_received();
int termination_signal_number();
std::string termination_signal_name(int signalNumber);
