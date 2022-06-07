#pragma once
#include <iostream>
#include <memory>
#include <vector>
#include <string>

#include <jni.h>

// from https://stackoverflow.com/questions/42930922/how-to-assign-a-string-literal
struct destroy_java_vm {
  void operator()(JavaVM* jvm)const{
    jvm->DestroyJavaVM();
  }
};
using up_java_vm = std::unique_ptr< JavaVM, destroy_java_vm >;

struct java_vm {
  up_java_vm vm;
  JNIEnv* env = 0;
};

using java_vm_options = std::vector<std::string>;

struct java_vm_init {
  unsigned version = JNI_VERSION_1_2;
  java_vm_options options;
  bool ignore_unrecognized = false;
  java_vm init() {
    std::vector<JavaVMOption> java_options(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
      java_options[i].optionString = &options[i][0];
    }
    JavaVMInitArgs args;
    args.version = version;
    args.options = java_options.data();
    args.nOptions = java_options.size();
    args.ignoreUnrecognized = false;
    java_vm retval;
    JavaVM* tmp = 0;
    auto res = JNI_CreateJavaVM(&tmp, (void **)&retval.env, &args);
    if (res < 0) {
      return {}; // error message?  How?
    }
    retval.vm.reset( tmp );
    return retval;
  }
};

extern java_vm_init g_jvm_init;
extern java_vm g_jvm;