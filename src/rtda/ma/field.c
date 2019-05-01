/*
 * Author: Jia Yang
 */

#include <stdlib.h>
#include "field.h"
#include "class.h"
#include "../heap/object.h"
#include "descriptor.h"
#include "../../symbol.h"
#include "resolve.h"
#include "../../utf8.h"


void field_init(Field *field, Class *c, BytecodeReader *reader)
{
    field->constant_value_index = INVALID_CONSTANT_VALUE_INDEX;
    field->clazz = c;
    field->deprecated = false;
    field->access_flags = readu2(reader);
    field->name = CP_UTF8(&(c->constant_pool), readu2(reader));
    field->descriptor = CP_UTF8(&(c->constant_pool), readu2(reader));

    char d = field->descriptor[0];
    if (d == 'J' || d == 'D') {
        field->category_two = true;
    } else {
        field->category_two = false;
    }

    field->type = NULL;

    // parse field's attributes
    u2 attr_count = readu2(reader);
    for (int i = 0; i < attr_count; i++) {
        const char *attr_name = CP_UTF8(&(c->constant_pool), readu2(reader));//rtcp_get_str(c->rtcp, readu2(reader));
        u4 attr_len = bcr_readu4(reader);

        if (S(Deprecated) == attr_name) {
            field->deprecated = true;
        } else if (S(ConstantValue) == attr_name) {
            /*
             * ConstantValue属性表示一个常量字段的值。
             * 在一个field_info结构的属性表中最多只能有一个ConstantValue属性。
             *
             * 非静态字段包含了ConstantValue属性，那么这个属性必须被虚拟机所忽略。
             */
            u2 index = readu2(reader);
            if (IS_STATIC(field->access_flags)) {  // todo
                field->constant_value_index = index;
//                field->v.static_value.u = resolve_single_constant(c, index);  // todo
            }
        } else if (S(Synthetic) == attr_name) {
            set_synthetic(&field->access_flags);
        } else if (S(Signature) == attr_name) {
            c->signature = CP_UTF8(&(c->constant_pool), readu2(reader));//rtcp_get_str(c->rtcp, readu2(reader));
        } else if (S(RuntimeVisibleAnnotations) == attr_name) { // ignore
//            u2 num = field->runtime_visible_annotations_num = readu2(reader);
//            field->runtime_visible_annotations = malloc(sizeof(struct annotation) * num);
//            CHECK_MALLOC_RESULT(field->runtime_visible_annotations);
//            for (u2 k = 0; k < num; k++) {
//                read_annotation(reader, field->runtime_visible_annotations + i);
//            }
            bcr_skip(reader, attr_len);
        } else if (S(RuntimeInvisibleAnnotations) == attr_name) { // ignore
//            u2 num = field->runtime_invisible_annotations_num = readu2(reader);
//            field->runtime_invisible_annotations = malloc(sizeof(struct annotation) * num);
//            CHECK_MALLOC_RESULT(field->runtime_invisible_annotations);
//            for (u2 k = 0; k < num; k++) {
//                read_annotation(reader, field->runtime_invisible_annotations + i);
//            }
            bcr_skip(reader, attr_len);
        } else {
            // unknown attribute
            bcr_skip(reader, attr_len);
        }
    }
}

bool field_is_accessible_to(const Field *field, const Class *visitor)
{
    // todo  实现对不对

    if (!class_is_accessible_to(field->clazz, visitor)) {
        return false;
    }

    if (field->clazz == visitor || IS_PUBLIC(field->access_flags))  // todo 对不对
        return true;

    if (IS_PRIVATE(field->access_flags)) {
        return false;
    }

    // 字段是protected，则只有 子类 和 同一个包下的类 可以访问
    if (IS_PROTECTED(field->access_flags)) {
        return class_is_subclass_of(visitor, field->clazz) || utf8_equals(field->clazz->pkg_name, visitor->pkg_name);
    }

    // 字段有默认访问权限（非public，非protected，也非private），则只有同一个包下的类可以访问
    return utf8_equals(field->clazz->pkg_name, visitor->pkg_name);
}

Object *field_get_type(Field *field)
{
    assert(field != NULL);

    if (field->type == NULL) {
        field->type = descriptor_to_type(field->clazz->loader, field->descriptor);
    }

    return field->type;
}

void field_release(Field *field)
{
    // todo
}

char *field_to_string(const Field *field)
{
#define MAX_LEN 1023 // big enough
    char *result = vm_malloc(sizeof(char)*(MAX_LEN + 1));

    if (field != NULL) {
        int n = snprintf(
                result, MAX_LEN, "field: %s~%s~%s", field->clazz->class_name, field->name, field->descriptor);
        if (n < 0) {
            jvm_abort("snprintf 出错\n"); // todo
        }
        assert(0 <= n && n <= MAX_LEN);
        result[n] = 0;
    } else {
        strcpy(result, "field: NULL");
    }

    return result;
}
