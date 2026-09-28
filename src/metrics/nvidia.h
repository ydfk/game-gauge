#pragma once
#include "common/types.h"
#include <windows.h>
namespace gauge {
class NvidiaProvider {
public:
    NvidiaProvider();
    ~NvidiaProvider();
    void sample(Hardware& hardware);
private:
    HMODULE module_{};
    bool initialized_{};
};
}

