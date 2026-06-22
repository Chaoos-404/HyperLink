#pragma once

#if defined(_WIN32) && defined(HYPERLINK_SHARED)
#if defined(HYPERLINK_BUILDING_LIBRARY)
#define HYPERLINK_API __declspec(dllexport)
#else
#define HYPERLINK_API __declspec(dllimport)
#endif
#elif defined(HYPERLINK_SHARED) && defined(__GNUC__)
#define HYPERLINK_API __attribute__((visibility("default")))
#else
#define HYPERLINK_API
#endif
