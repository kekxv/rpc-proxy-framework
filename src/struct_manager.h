#ifndef STRUCT_MANAGER_H
#define STRUCT_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <json/json.h>
#include <ffi.h>

using json = Json::Value;

// 描述一个结构体成员的内存布局
struct StructMember {
    std::string name;
    std::string type_name;
    ffi_type* ffi_type_ptr;
    size_t size;
    size_t alignment;
    size_t offset;
};

// 描述一个完整结构体的内存布局和FFI类型信息
struct StructLayout {
    std::string name;
    std::vector<StructMember> members;
    std::unique_ptr<ffi_type> ffi_type_struct;
    std::unique_ptr<ffi_type*[]> ffi_elements; // To keep element arrays alive
    size_t total_size;
    size_t alignment;
};

class StructManager {
public:
    StructManager();

    // 根据JSON定义注册一个新的结构体
    void register_struct(const std::string& name, const json& definition);

    // 取消注册一个结构体
    void unregister_struct(const std::string& name);

    // 获取已注册结构体的布局信息
    const StructLayout* get_layout(const std::string& name) const;

    // 判断一个类型名是否是已注册的结构体
    bool is_struct(const std::string& type_name) const;

    // 将内存中的结构体序列化为JSON对象
    Json::Value serializeStruct(const std::string& struct_name, const void* struct_ptr) const;

private:
    // 将字符串类型名映射到FFI类型和尺寸/对齐信息
    ffi_type* get_ffi_type_from_string(const std::string& type_name);
    size_t get_size_from_string(const std::string& type_name);
    size_t get_alignment_from_string(const std::string& type_name);

    std::map<std::string, ffi_type*> basic_type_map;
    std::map<std::string, StructLayout> registered_structs;
};

#endif // STRUCT_MANAGER_H
