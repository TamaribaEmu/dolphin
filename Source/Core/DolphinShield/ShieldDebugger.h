#pragma once

#include <cstddef>

void ShieldDebuggerSetEnabled(bool enabled);
void ShieldDebuggerSample();
int ShieldDebuggerCommand(const char* command, char* response, std::size_t response_size);
