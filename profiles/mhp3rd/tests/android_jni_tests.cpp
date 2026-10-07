// JNI transport contracts with a synthetic JVM, without an Android activity.
#include "platform/android_jni.hpp"

#include <jni.h>

#include <cstdarg>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
JNIEnv environment;
JNINativeInterface_ methods{};
bool no_environment{}, no_activity{}, no_class{}, no_method{}, throws{}, no_result{}, exception{};
unsigned releases{};
std::string argument;
std::string result = "日本語 mod";

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
jclass JNICALL get_class(JNIEnv *, jobject) {
    return no_class ? nullptr : reinterpret_cast<jclass>(1);
}
void JNICALL delete_ref(JNIEnv *, jobject) {}
jmethodID JNICALL get_method(JNIEnv *, jclass, const char *name, const char *signature) {
    require(std::strcmp(name, "documentName") == 0, "correct activity method");
    require(std::strcmp(signature, "(Ljava/lang/String;)Ljava/lang/String;") == 0, "correct JNI signature");
    exception = no_method;
    return no_method ? nullptr : reinterpret_cast<jmethodID>(1);
}
jstring JNICALL new_string(JNIEnv *, const char *text) {
    argument = text;
    return reinterpret_cast<jstring>(&argument);
}
jobject JNICALL call_method(JNIEnv *, jclass, jmethodID, va_list arguments) {
    const auto passed = va_arg(arguments, jstring);
    require(
        *reinterpret_cast<std::string *>(passed) == "content://provider/document/opaque-id", "URI is passed unchanged");
    exception = throws;
    return no_result ? nullptr : reinterpret_cast<jobject>(&result);
}
jboolean JNICALL check_exception(JNIEnv *) {
    return exception ? JNI_TRUE : JNI_FALSE;
}
void JNICALL clear_exception(JNIEnv *) {
    exception = false;
}
const char *JNICALL get_chars(JNIEnv *, jstring string, jboolean *) {
    return reinterpret_cast<std::string *>(string)->c_str();
}
void JNICALL release_chars(JNIEnv *, jstring, const char *) {
    ++releases;
}
}

extern "C" void *SDL_GetAndroidJNIEnv() {
    return no_environment ? nullptr : &environment;
}
extern "C" void *SDL_GetAndroidActivity() {
    return no_activity ? nullptr : reinterpret_cast<void *>(1);
}

int main() {
    try {
        methods.GetObjectClass = get_class;
        methods.DeleteLocalRef = delete_ref;
        methods.GetStaticMethodID = get_method;
        methods.NewStringUTF = new_string;
        methods.CallStaticObjectMethodV = call_method;
        methods.ExceptionCheck = check_exception;
        methods.ExceptionClear = clear_exception;
        methods.GetStringUTFChars = get_chars;
        methods.ReleaseStringUTFChars = release_chars;
        environment.functions = &methods;
        const auto read = [] { return mhp3rd::android::document_name("content://provider/document/opaque-id"); };
        require(read() == result && releases == 1, "provider metadata is returned and JVM text released");
        no_environment = true;
        require(!read(), "missing JVM returns no name");
        no_environment = false;
        no_activity = true;
        require(!read(), "missing activity returns no name");
        no_activity = false;
        no_class = true;
        require(!read(), "missing class returns no name");
        no_class = false;
        no_method = true;
        require(!read() && !exception, "missing metadata method clears its exception");
        no_method = false;
        throws = true;
        require(!read() && !exception, "provider exception is cleared");
        throws = false;
        no_result = true;
        require(!read(), "missing provider name is preserved");
        std::cout << "Android JNI contracts passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
