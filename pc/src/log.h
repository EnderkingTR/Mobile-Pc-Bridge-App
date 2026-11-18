#pragma once
#include <cstdio>

#define LOGI(...) do { std::fprintf(stderr, "[kopru] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOGW(...) do { std::fprintf(stderr, "[kopru] UYARI: " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOGE(...) do { std::fprintf(stderr, "[kopru] HATA: " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
