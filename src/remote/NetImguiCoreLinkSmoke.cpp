#include "remote/NetImguiHost.hpp"

// This executable is a build/link probe only. It is intentionally not added to
// CTest: constructing the host opens a TCP listener. The argc branch prevents
// normal accidental execution while keeping real references to the embedded
// server core in the final link.
int main(int argc, char**) {
    if (argc == 4242) {
        reg::remote::NetImguiHost host;
        host.update(1280U, 720U, true);
        return host.status().listening ? 0 : 1;
    }
    return 0;
}
