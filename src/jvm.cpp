/*
 * Author: kayo
 */

#include <dirent.h>
#include <sys/stat.h>
#include <ctime>
#include "jvm.h"
#include "loader/ClassLoader.h"
#include "rtda/thread/Thread.h"
#include "rtda/ma/Access.h"
#include "interpreter/interpreter.h"
#include "rtda/heap/StrPool.h"
#include "symbol.h"

using namespace std;

HeapMgr g_heap_mgr;

VMEnv vmEnv;

VMEnv::VMEnv()
{
    strPool = new StrPool;
}

void init_symbol();

// main thread is current thread
static void initMainThread()
{
    auto mainThread = new Thread(nullptr);

    Class *jltgClass = loadSysClass(S(java_lang_ThreadGroup));
    vmEnv.sysThreadGroup = Object::newInst(jltgClass);

    // 初始化 system_thread_group
    // java/lang/ThreadGroup 的无参数构造函数主要用来：
    // Creates an empty Thread group that is not in any Thread group.
    // This method is used to create the system Thread group.
    jltgClass->clinit();
    execJavaFunc(jltgClass->getConstructor(S(___V)), vmEnv.sysThreadGroup);

    mainThread->setThreadGroupAndName(vmEnv.sysThreadGroup, MAIN_THREAD_NAME);
}

static void *execGCThread(void *arg)
{
    // todo
    return nullptr;
}

// create gc thread
static void createGCThread()
{
    // todo
    pthread_t pid;
    int ret = pthread_create(&pid, NULL, execGCThread, nullptr);
    if (ret != 0) {
        vm_internal_error("create Thread failed");
    }
}

static void startJvm(const char *main_class_name)
{
    init_symbol();
    init_thread_module();

    // create system class loader
    auto loader = new ClassLoader(true);

    initMainThread();
    createGCThread();

    // 先加载 sun.mis.VM 类，然后执行其类初始化方法
    Class *vm_class = loadSysClass("sun/misc/VM");
    if (vm_class == nullptr) {
        jvm_abort("vm_class is null\n");  // todo throw exception
        return;
    }

    // VM类的 "initialize~()V" 方法需调用执行
    // 在VM类的类初始化方法中调用了 "initialize" 方法。
    vm_class->clinit();

    Class *main_class = loader->loadClass(main_class_name);
    Method *main_method = main_class->lookupStaticMethod(S(main), S(_array_java_lang_String__V));
    if (main_method == nullptr) {
        jvm_abort("can't find method main."); // todo
    } else {
        if (!main_method->isPublic()) {
            jvm_abort("method main must be public."); // todo
        }
        if (!main_method->isStatic()) {
            jvm_abort("method main must be static."); // todo
        }
    }

    // 开始在主线程中执行 main 方法
    execJavaFunc(main_method, (Object *) nullptr); //  todo


    // todo 如果有其他的非后台线程在执行，则main线程需要在此wait

    // todo main_thread 退出，做一些清理工作。
}

static void findJars(const char *path, vector<std::string> &result)
{
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        printvm("open dir failed. %s\n", path);
    }

    struct dirent *entry;
    struct stat statbuf;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char abspath[PATH_MAX];
        // sprintf 和 snprintf 会自动在加上字符串结束符'\0'
        sprintf(abspath, "%s/%s", path, entry->d_name); // 绝对路径

        stat(abspath, &statbuf);
        if (S_ISREG(statbuf.st_mode)) { // 常规文件
            char *suffix = strrchr(abspath, '.');
            if (suffix != nullptr && strcmp(suffix, ".jar") == 0)
                result.emplace_back(abspath);
        }
    }

    closedir(dir);
}

int main(int argc, char* argv[])
{
    time_t time0;
    time(&time0);

    char bootstrap_classpath[PATH_MAX] = { 0 };
    char extension_classpath[PATH_MAX] = { 0 };
    char user_classpath[PATH_MAX] = { 0 };

    char main_class_name[FILENAME_MAX] = { 0 };

    // parse cmd arguments
    // 可执行程序的名字为 argv[0]，跳过。
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            const char *name = argv[i];
            if (strcmp(name, "-bcp") == 0) { // parse Bootstrap Class Path
                if (++i >= argc) {
                    jvm_abort("缺少参数：%s\n", name);
                }
                strcpy(bootstrap_classpath, argv[i]);
            } else if (strcmp(name, "-cp") == 0) { // parse Class Path
                if (++i >= argc) {
                    jvm_abort("缺少参数：%s\n", name);
                }
                strcpy(user_classpath, argv[i]);
            } else {
                jvm_abort("unknown 参数: %s\n", name);
            }
        } else {
            strcpy(main_class_name, argv[i]);
        }
    }

    if (main_class_name[0] == 0) {  // empty
        jvm_abort("no input file\n");
    }

    // 如果 main_class_name 有 .class 后缀，去掉后缀。
    char *p = strrchr(main_class_name, '.');
    if (p != nullptr && strcmp(p, ".class") == 0) {
        *p = 0;
    }

    // parse bootstrap classpath
    if (bootstrap_classpath[0] == 0) { // empty
        // 命令行参数没有设置 bootstrap_classpath 的值，那么使用 JAVA_HOME 环境变量
        char *javaHome = getenv("JAVA_HOME"); // JAVA_HOME 是 JDK 的目录
        if (javaHome == nullptr) {
            vm_internal_error("no java lib"); // todo
        }
        strcpy(bootstrap_classpath, javaHome);
        strcat(bootstrap_classpath, "/jre/lib");
    }

    findJars(bootstrap_classpath, vmEnv.jreLibJars);

    // 第0个位置放rt.jar，因为rt.jar常用，所以放第0个位置首先搜索。
    for (auto iter = vmEnv.jreLibJars.begin(); iter != vmEnv.jreLibJars.end(); iter++) {
        auto i = iter->rfind('\\');
        auto j = iter->rfind('/');
        if ((i != iter->npos && iter->compare(i + 1, 6, "rt.jar") == 0)
                || (j != iter->npos && iter->compare(j + 1, 6, "rt.jar") == 0)) {
            std::swap(*(vmEnv.jreLibJars.begin()), *iter);
            break;
        }
    }

    // parse extension classpath
    if (extension_classpath[0] == 0) {  // empty
        strcpy(extension_classpath, bootstrap_classpath);
        strcat(extension_classpath, "/ext");  // todo JDK9+ 的目录结构有变动！！！！！！！
    }

    findJars(extension_classpath, vmEnv.jreExtJars);

    // parse user classpath
    if (user_classpath[0] == 0) {  // empty
        char *classpath = getenv("CLASSPATH");
        if (classpath == nullptr) {
            char buf[PATH_MAX + 1];
            getcwd(buf, PATH_MAX); // current working path
            vmEnv.userDirs.emplace_back(buf);
        } else {
            vmEnv.userDirs.emplace_back(classpath);
        }
    } else {
        const char *delim = ";"; // 各个path以分号分隔
        char *path = strtok(user_classpath, delim);
        while (path != nullptr) {
            const char *suffix = strrchr(path, '.');
            if (suffix != nullptr && strcmp(suffix, ".jar") == 0) { // jar file
                vmEnv.userJars.emplace_back(path);
            } else { // directory
                vmEnv.userDirs.emplace_back(path);
            }
            path = strtok(nullptr, delim);
        }
    }

    register_all_native_methods(); // todo 不要一次全注册，需要时再注册

    time_t time2;
    time(&time2);
    
    startJvm(main_class_name);

    time_t time3;
    time(&time3);
    printf("run KayoVM: %lds\n", ((long)(time3)) - ((long)(time2)));
    return 0;
}

void vm_internal_error(const char *msg)
{
    assert(msg != nullptr);
    // todo
    jvm_abort(msg);
}

void vm_out_of_memory_error(const char *msg)
{
    assert(msg != nullptr);
    // todo
    jvm_abort(msg);
}

void vm_stack_overflow_error()
{
    // todo
    jvm_abort("vm_stack_overflow_error");
}

void vm_unknown_error(const char *msg)
{
    assert(msg != nullptr);
    // todo
    jvm_abort(msg);
}

