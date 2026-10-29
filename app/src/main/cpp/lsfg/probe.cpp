// Checks a Lossless.dll: reads the shaders out of it and translates them to SPIR-V, the same way the frame
// generation does at run time. Used by the "Check Lossless.dll" button; needs no GPU.
#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "extract.hpp"
#include "trans.hpp"

namespace {
std::string run(const std::string &path) {
    std::ostringstream o;
    try {
        Extract::setDllPath(path);
        auto t0 = std::chrono::steady_clock::now();
        Extract::extractShaders();
        auto names = Extract::shaderNames();
        size_t dxbcBytes = 0, spvBytes = 0;
        int ok = 0, bad = 0;
        std::set<std::string> caps;
        std::string firstErr;
        for (const auto &n : names) {
            try {
                auto dx = Extract::getShader(n);
                dxbcBytes += dx.size();
                auto sp = Extract::translateShader(dx);
                spvBytes += sp.size();
                ok++;
                const uint32_t *w = reinterpret_cast<const uint32_t *>(sp.data());
                size_t cnt = sp.size() / 4;
                for (size_t i = 5; i < cnt;) {
                    uint32_t op = w[i] & 0xFFFF, len = w[i] >> 16;
                    if (len == 0) break;
                    if (op == 17 && i + 1 < cnt) {            // OpCapability
                        uint32_t c = w[i + 1];
                        if (c == 5345) caps.insert("VulkanMemoryModel");
                        else if (c == 9) caps.insert("Float16");
                        else if (c == 22) caps.insert("Int16");
                        else if (c == 11) caps.insert("Int64");
                        else if (c == 4433) caps.insert("StorageBuffer16BitAccess");
                    }
                    if (op == 54) break;                       // OpFunction: capabilities come before
                    i += len;
                }
            } catch (const std::exception &e) {
                bad++;
                if (firstErr.empty()) firstErr = n + ": " + e.what();
            } catch (...) {
                bad++;
                if (firstErr.empty()) firstErr = n + ": unknown error";
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        o << "Lossless.dll read: " << names.size() << " shaders found, " << (dxbcBytes / 1024) << " KB of DXBC.\n";
        o << "Translated to SPIR-V: " << ok << " ok, " << bad << " failed (" << ms << " ms, " << (spvBytes / 1024) << " KB).\n";
        if (!firstErr.empty()) o << "First failure: " << firstErr << "\n";
        o << "Needs on the GPU:";
        if (caps.empty()) o << " nothing special";
        for (const auto &c : caps) o << " " << c;
        o << "\n";
    } catch (const std::exception &e) {
        o << "Could not use this file: " << e.what() << "\n";
    } catch (...) {
        o << "Could not use this file (unknown error).\n";
    }
    return o.str();
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Lsfg_probe(JNIEnv *env, jobject, jstring jpath) {
    const char *p = env->GetStringUTFChars(jpath, nullptr);
    std::string path = p ? p : "";
    if (p) env->ReleaseStringUTFChars(jpath, p);
    std::string r = run(path);
    __android_log_print(ANDROID_LOG_INFO, "GuestHost", "lsfg: probe: %s", r.c_str());
    return env->NewStringUTF(r.c_str());
}
