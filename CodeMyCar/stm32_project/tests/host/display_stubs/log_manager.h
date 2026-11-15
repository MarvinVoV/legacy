#pragma once
#define LOG_ERROR(module, ...) ((void)0)
#define LOG_INFO(module, ...) do { if (0) printf(__VA_ARGS__); } while (0)
#include <stdio.h>
