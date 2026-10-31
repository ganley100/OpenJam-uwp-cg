// NBA JAM: On Fire Edition - how busy each thread is
//
// See thread_load.cpp.

#pragma once

#include <string>

namespace rex {
class Runtime;
}

// The runtime whose threads are measured. Once, after setup.
void NbaThreadLoadUse(rex::Runtime* runtime);

// The busiest threads since the last call, as "name 97%, name 41%, ...":
// the share of one CPU core each used.
std::string NbaThreadLoad();
