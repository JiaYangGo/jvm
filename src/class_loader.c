#include <dirent.h>
#include <minizip/unzip.h>

#include "cabin.h"
#include "hash.h"
#include "object.h"
#include "class_loader.h"
#include "interpreter.h"
#include "encoding.h"


#define JDK_MODULES_MAX_COUNT 512 // big enough
static char *jdk_modules[JDK_MODULES_MAX_COUNT] = { NULL };

static void findFilesBySuffix(const char *path, const char *suffix, char **result)
{
    // path curr_path(path);
    // if (!exists(curr_path)) {
    //     // todo error
    //     return;
    // }

    // directory_entry entry(curr_path);
    // if (entry.status().type() != file_type::directory) {
    //     // todo error
    //     return;
    // }

    // directory_iterator files(curr_path);
    // for (auto& f: files) {
    //     if (f.is_regular_file()) {
    //         char abspath[PATH_MAX];
    //         // sprintf 和 snprintf 会自动在加上字符串结束符'\0'
    //         sprintf(abspath, "%s/%s", path0, f.path().filename().string().c_str()); // 绝对路径

    //         char *tmp = strrchr(abspath, '.');
    //         if (tmp != NULL && strcmp(++tmp, suffix) == 0) {
    //             *result++ = strdup(abspath);
    //             // result.emplace_back(abspath);
    //         }
    //     }
    // }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        // todo error
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char abspath[PATH_MAX];
        // sprintf 和 snprintf 会自动在加上字符串结束符'\0'
        snprintf(abspath, PATH_MAX, "%s/%s", path, entry->d_name); // 绝对路径

        // check suffix
        char *tmp = strrchr(abspath, '.');
        if (tmp != NULL && strcmp(++tmp, suffix) == 0) {
            *result++ = strdup(abspath);
        }
    }
}

static char bootstrap_classpath[PATH_MAX] = { 0 };
static char classpath[PATH_MAX] = { 0 };

void set_classpath(const char *cp)
{
    assert(cp != NULL);
    strcpy(classpath, cp);
}

const char *get_classpath()
{
    return classpath;
}

void set_bootstrap_classpath(const char *bcp)
{
    assert(bcp != NULL);
    strcpy(bootstrap_classpath, bcp);
}

static void init_classpath()
{
    assert(g_java_home[0] != 0);

    char home[PATH_MAX];
    snprintf(home, PATH_MAX, "%s/jmods", g_java_home);
    findFilesBySuffix(home, "jmod", jdk_modules);

    if (classpath[0] == 0) {  // empty
        char *cp = getenv("CLASSPATH");
        if (cp != NULL) {
            strcpy(classpath, cp);
        } else {
            // todo error. no CLASSPATH！
            JVM_PANIC("error. no CLASSPATH！");
        }
    }
//    else {
//        const char *delim = ";"; // 各个path以分号分隔
//        char *path = strtok(user_classpath, delim);
//        while (path != NULL) {
//            const char *suffix = strrchr(path, '.');
//            if (suffix != NULL && strcmp(suffix, ".jar") == 0) { // jar file
//                userJars.emplace_back(path);
//            } else { // directory
//                userDirs.emplace_back(path);
//            }
//            path = strtok(NULL, delim);
//        }
//    }
}

typedef enum ClassLocation {
    IN_JAR,
    IN_MODULE
} ClassLocation;

static int filename_compare_func(unzFile file, const char *filename1, const char *filename2)
{
    return strcmp(filename1, filename2);
}

/*
 * @param class_name: xxx/xxx/xxx
 */
TJE static u1 *read_class(const char *path, const char *class_name, ClassLocation location, size_t *bytecode_len)
{
    assert(path != NULL && class_name != NULL && bytecode_len != NULL);

    unzFile module_file = unzOpen64(path);
    if (module_file == NULL) {
        // throw java_io_IOException(string("unzOpen64 failed: ") + path);
        raise_exception(S(java_io_IOException), NULL); // todo msg   
    }

    if (unzGoToFirstFile(module_file) != UNZ_OK) {
        unzClose(module_file);
        // throw java_io_IOException(string("unzGoToFirstFile failed: ") + path);
        raise_exception(S(java_io_IOException), NULL); // todo msg   
    }

    char buf[strlen(class_name) + 32]; // big enough
    if (location == IN_JAR) {
        strcat(strcpy(buf, class_name), ".class");
    } else if (location == IN_MODULE) {
        // All classes 放在 module 的 "classes" 目录下
        strcat(strcat(strcpy(buf, "classes/"), class_name), ".class");
    } else {
        JVM_PANIC("never goes here."); // todo
    }
// typedef int (*unzFileNameComparer)(unzFile file, const char *filename1, const char *filename2);
//    int k = unzLocateFile(module_file, buf, 1);
    int k  = unzLocateFile(module_file, buf, filename_compare_func);
    if (k != UNZ_OK) {
        // not found
        unzClose(module_file);
        return NULL;
    }

    // find out!
    if (unzOpenCurrentFile(module_file) != UNZ_OK) {
        unzClose(module_file);
        // throw java_io_IOException(string("unzOpenCurrentFile failed: ") + path);
        raise_exception(S(java_io_IOException), NULL); // todo msg   
    }

    unz_file_info64 file_info;
    unzGetCurrentFileInfo64(module_file, &file_info, buf, sizeof(buf), NULL, 0, NULL, 0);

    u1 *bytecode = vm_malloc(sizeof(u1) * file_info.uncompressed_size);  // new u1[file_info.uncompressed_size];
    int size = unzReadCurrentFile(module_file, bytecode, (unsigned int) (file_info.uncompressed_size));
    unzCloseCurrentFile(module_file);
    unzClose(module_file);
    if (size != (int) file_info.uncompressed_size) {
        // throw java_io_IOException(string("unzReadCurrentFile failed: ") + path);
        raise_exception(S(java_io_IOException), NULL); // todo msg   
    }

    *bytecode_len = file_info.uncompressed_size;
    return bytecode;
    // return make_pair(bytecode, file_info.uncompressed_size);
}

/*
 * Read JDK 类库中的类，不包括Array Class.
 * xxx/xxx/xxx
 */
static u1 *read_boot_class(const utf8_t *class_name, size_t *bytecode_len)
{
    assert(class_name != NULL && bytecode_len != NULL);
//    assert(isSlashName(class_name));
    assert(class_name[0] != '['); // don't load array class

    for (char **modules = jdk_modules; *modules != NULL; modules++) {
        u1 *bytecode = read_class(*modules, class_name, IN_MODULE, bytecode_len);
        if (bytecode != NULL) { // find out
            return bytecode;
        }
    }

    return NULL;
}


static PHS boot_packages;

// <const utf8_t *, Class *>
static PHM boot_classes;  // todo 加读写锁

// vm中所有存在的 class loaders，include "boot class loader".
// const Object *
static PHS loaders;

static void add_class_to_class_loader(Object *class_loader, Class *c)
{
    assert(c != NULL);
    if (class_loader == BOOT_CLASS_LOADER) {
        phm_insert(&boot_classes, c->class_name, c);
        return;
    }

    phs_add(&loaders, class_loader);

    if (class_loader->classes == NULL) {
        class_loader->classes = phm_create((point_hash_func)utf8_hash, (point_equal_func)utf8_equals);
    }
    phm_insert(class_loader->classes, c->class_name, c);

    // Invoked by the VM to record every loaded class with this loader.
    // void addClass(Class<?> c);
//    Method *m = classLoader->clazz->getDeclaredInstMethod("addClass", "Ljava/lang/Class;");
//    assert(m != NULL);
//    execJavaFunc(m, { (slot_t) classLoader, (slot_t) c });
}

static void inject_fields(Class *c)
{
    assert(c != NULL);

    if (utf8_equals(c->class_name, S(java_lang_invoke_MemberName))) {
        //@Injected intptr_t vmindex; // vtable index or offset of resolved member
        bool b = inject_inst_field(c, "vmindex", S(I));
        if (!b) {
            JVM_PANIC("inject fields error"); // todo
        }
        return;
    }

    if (utf8_equals(c->class_name, "java/lang/invoke/ResolvedMethodName")) {
        //@Injected JVM_Method* vmtarget;
        //@Injected Class<?>    vmholder;
        bool b1 = inject_inst_field(c, "vmtarget", S(sig_java_lang_Object));
        bool b2 = inject_inst_field(c, "vmholder", S(sig_java_lang_Class));
        if (!b1 || !b2) {
            JVM_PANIC("inject fields error"); // todo
        }
        return;
    }
}

Class *define_prim_type_class(const char *class_name);
Class *define_array_class(Object *loader, const char *class_name);

Class *load_boot_class(const utf8_t *name)
{
    assert(name != NULL);
    assert(IS_SLASH_CLASS_NAME(name));
    assert(name[0] != '['); // don't load array class

    Class *found = (Class *) phm_find(&boot_classes, name);
    if (found != NULL) {
        TRACE("find loaded class (%s) from pool.", name);
        return found;
    }

    Class *c = NULL;
    if (is_prim_class_name(name)) {
        c = define_prim_type_class(name);
    } else {
        size_t bytecode_len;
        u1 *bytecode = read_boot_class(name, &bytecode_len);
        if (bytecode != NULL) { // find out
            c = define_class(BOOT_CLASS_LOADER, bytecode, bytecode_len);
        }
    }

    if (c != NULL) {
        // boot_packages.insert(c->pkg_name);        
        phs_add(&boot_packages, c->pkg_name);   
        inject_fields(c);
        add_class_to_class_loader(BOOT_CLASS_LOADER, c);
    }
    
    return c;
}

Class *load_array_class(Object *loader, const utf8_t *arr_class_name)
{
    assert(arr_class_name != NULL);
    assert(arr_class_name[0] == '['); // must be array class name

    const char *ele_class_name = arr_class_name_to_ele_class_name(arr_class_name);
    Class *c = load_class(loader, ele_class_name);
    if (c == NULL)
        return NULL; // todo

    /* Array Class 用它的元素的类加载器加载 */

    Class *arr_class = find_loaded_class(c->loader, arr_class_name);
    if (arr_class != NULL)
        return arr_class; // find out
    
    arr_class = define_array_class(c->loader, arr_class_name);
    assert(arr_class != NULL);
    if (arr_class->loader == BOOT_CLASS_LOADER) {
        // boot_packages.insert(arr_class->pkg_name); // todo array class 的pkg_name是啥
        phs_add(&boot_packages, arr_class->pkg_name);    // todo array class 的pkg_name是啥
    }
    add_class_to_class_loader(arr_class->loader, arr_class);
    return arr_class;
}

Class *load_type_array_class(ArrayType type)
{
    const char *arr_class_name = NULL;

    switch (type) {
        case JVM_AT_BOOLEAN: arr_class_name = S(array_Z); break;
        case JVM_AT_CHAR:    arr_class_name = S(array_C); break;
        case JVM_AT_FLOAT:   arr_class_name = S(array_F); break;
        case JVM_AT_DOUBLE:  arr_class_name = S(array_D); break;
        case JVM_AT_BYTE:    arr_class_name = S(array_B); break;
        case JVM_AT_SHORT:   arr_class_name = S(array_S); break;
        case JVM_AT_INT:     arr_class_name = S(array_I); break;
        case JVM_AT_LONG:    arr_class_name = S(array_J); break;
        default:
            raise_exception(S(java_lang_UnknownError), NULL); // todo msg
            // throw java_lang_UnknownError("Invalid array type: " + to_string(type));
    }

    return load_array_class(BOOT_CLASS_LOADER, arr_class_name);
}

const utf8_t *get_boot_package(const utf8_t *name)
{
    return (const utf8_t *) phs_find(&boot_packages, name);
}

PHS *get_boot_packages()
{
    return &boot_packages;
}

Class *find_loaded_class(Object *class_loader, const utf8_t *name)
{
    assert(name != NULL);
    assert(IS_SLASH_CLASS_NAME(name));

    if (class_loader == BOOT_CLASS_LOADER) {
        return (Class *) phm_find(&boot_classes, name);
    }

    // is not boot classLoader
    if (class_loader->classes != NULL) {
        return (Class *) phm_find(class_loader->classes, name);
    }

    // not find
    return NULL;
}

Class *load_class(Object *class_loader, const utf8_t *name)
{
    assert(name != NULL);
//    assert(isSlashName(name));

    utf8_t *slash_name = dot_to_slash_dup(name);
    Class *c = find_loaded_class(class_loader, slash_name);
    if (c != NULL)
        return c;

    if (slash_name[0] == '[')
        return load_array_class(class_loader, slash_name);

    // 先尝试用boot class loader load the class
    c = load_boot_class(slash_name);
    if (c != NULL || class_loader == NULL)
        return c;

    // todo 再尝试用扩展classLoader load the class

    // public Class<?> loadClass(String name) throws ClassNotFoundException
    Method *m = lookup_inst_method(class_loader->clazz, S(loadClass),
                                   S(_java_lang_String__java_lang_Class));
    assert(m != NULL);

    utf8_t *dot_name = slash_to_dot_dup(name);
    slot_t *slot = exec_java(m, (slot_t[]) { rslot(class_loader), rslot(alloc_string(dot_name)) });
    assert(slot != NULL);
    jclsRef co = slot_get_ref(slot);
    assert(co != NULL && co->jvm_mirror != NULL);
    c = co->jvm_mirror;
    add_class_to_class_loader(class_loader, c);
    // init_class(c); /////////// todo /////////////////////////////////////////
    return c;
}

Class *define_class1(jref class_loader, jref name,
                     jarrRef bytecode, jint off, jint len, jref protection_domain, jref source)
{
    u1 *data = (u1 *) bytecode->data;
    Class *c = define_class(class_loader, data + off, len);
    // c->class_name和name是否相同 todo
//    printvm("class_name: %s\n", c->class_name);
    return c;
}

Class *init_class(Class *c)
{
    assert(c != NULL);

    if (c->inited) {
        return c;
    }

    pthread_mutex_lock(&c->clinit_mutex);
    if (c->inited) { // 需要再次判断 inited，有可能被其他线程置为 true
        pthread_mutex_unlock(&c->clinit_mutex);
        return c;
    }

    c->state = CLASS_INITING;

    if (c->super_class != NULL) {
        init_class(c->super_class);
    }

    // 在这里先行 set inited true, 如不这样，后面执行<clinit>时，
    // 可能调用putstatic等函数也会触发<clinit>的调用造成死循环。
    c->inited = true;

    Method *m = get_declared_method_noexcept(c, S(class_init), S(___V));
    if (m != NULL) { // 有的类没有<clinit>方法
        exec_java(m, NULL);
    }

    c->inited = true;
    c->state = CLASS_INITED;
    pthread_mutex_unlock(&c->clinit_mutex);

    return c;
}

Class *link_class(Class *c)
{
    assert(c != NULL);

    // todo

    c->state = CLASS_LINKED;
    return c;
}

Object *get_platform_class_loader()
{
    Class *c = load_boot_class(S(java_lang_ClassLoader));
    assert(c != NULL);

    // public static ClassLoader getPlatformClassLoader();
    Method *get = get_declared_static_method(c, S(getPlatformClassLoader), S(___java_lang_ClassLoader));
    return exec_java_r(get, NULL);
}

Object *get_app_class_loader()
{
    Class *c = load_boot_class(S(java_lang_ClassLoader));
    assert(c != NULL);

    // public static ClassLoader getSystemClassLoader();
    Method *get = get_declared_static_method(c, S(getSystemClassLoader), S(___java_lang_ClassLoader));
    return exec_java_r(get, NULL);
}

Class *g_object_class = NULL;
Class *g_class_class = NULL;
Class *g_string_class = NULL;

void init_class_loader()
{
    init_classpath();
    
    phs_init(&loaders, NULL, NULL);
    phs_init(&boot_packages, (point_hash_func) utf8_hash, (point_equal_func) utf8_equals);
    phm_init(&boot_classes, (point_hash_func) utf8_hash, (point_equal_func) utf8_equals);

    g_object_class = load_boot_class(S(java_lang_Object));
    g_class_class = load_boot_class(S(java_lang_Class));

    // g_class_class 至此创建完成。
    // 在 g_class_class 创建完成之前创建的 Class 都没有设置 java_mirror 字段，现在设置下。

    phm_touch_values(&boot_classes, (void (*)(void *)) gen_class_object);

    g_string_class = load_boot_class(S(java_lang_String));
    // build string pool
    g_string_class->string.str_pool = phs_create((point_hash_func) string_hash, (point_equal_func) string_equals);
    // g_string_class->str_pool = new unordered_set<Object *, StringHash, StringEquals>;
    // g_string_class->buildStrPool();

    phs_add(&loaders, BOOT_CLASS_LOADER);
}

PHM *get_all_boot_classes()
{
    return &boot_classes;
}

PHS *get_all_class_loaders()
{
    return &loaders;
}