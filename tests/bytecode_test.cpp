#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/vm/bytecode.hpp"
#include "roxy/core/vector.hpp"

using namespace rx;

TEST_CASE("Opcode encoding and decoding") {
    SUBCASE("Format A: 3-operand instructions") {
        u32 instr = encode_abc(Opcode::ADD_I, 1, 2, 3);

        CHECK(decode_opcode(instr) == Opcode::ADD_I);
        CHECK(decode_a(instr) == 1);
        CHECK(decode_b(instr) == 2);
        CHECK(decode_c(instr) == 3);
    }

    SUBCASE("Format B: immediate instructions") {
        u32 instr = encode_abi(Opcode::LOAD_INT, 5, 1000);

        CHECK(decode_opcode(instr) == Opcode::LOAD_INT);
        CHECK(decode_a(instr) == 5);
        CHECK(decode_imm16(instr) == 1000);
    }

    SUBCASE("Format B: negative immediate") {
        u32 instr = encode_abi(Opcode::LOAD_INT, 5, static_cast<u16>(static_cast<i16>(-100)));

        CHECK(decode_opcode(instr) == Opcode::LOAD_INT);
        CHECK(decode_a(instr) == 5);
        CHECK(static_cast<i16>(decode_imm16(instr)) == -100);
    }

    SUBCASE("Format C: offset instructions") {
        u32 instr = encode_aoff(Opcode::JMP_IF, 10, -50);

        CHECK(decode_opcode(instr) == Opcode::JMP_IF);
        CHECK(decode_a(instr) == 10);
        CHECK(decode_offset(instr) == -50);
    }

    SUBCASE("Format C: positive offset") {
        u32 instr = encode_aoff(Opcode::JMP, 0, 100);

        CHECK(decode_opcode(instr) == Opcode::JMP);
        CHECK(decode_offset(instr) == 100);
    }
}

TEST_CASE("Opcode to string") {
    CHECK(strcmp(opcode_to_string(Opcode::LOAD_NULL), "LOAD_NULL") == 0);
    CHECK(strcmp(opcode_to_string(Opcode::ADD_I), "ADD_I") == 0);
    CHECK(strcmp(opcode_to_string(Opcode::JMP), "JMP") == 0);
    CHECK(strcmp(opcode_to_string(Opcode::RET), "RET") == 0);
    CHECK(strcmp(opcode_to_string(Opcode::CALL), "CALL") == 0);
}

TEST_CASE("BCConstant creation") {
    SUBCASE("Null constant") {
        BCConstant c = BCConstant::make_null();
        CHECK(c.type == BCConstant::Null);
    }

    SUBCASE("Bool constant") {
        BCConstant c_true = BCConstant::make_bool(true);
        CHECK(c_true.type == BCConstant::Bool);
        CHECK(c_true.as_bool == true);

        BCConstant c_false = BCConstant::make_bool(false);
        CHECK(c_false.type == BCConstant::Bool);
        CHECK(c_false.as_bool == false);
    }

    SUBCASE("Int constant") {
        BCConstant c = BCConstant::make_int(123456789LL);
        CHECK(c.type == BCConstant::Int);
        CHECK(c.as_int == 123456789LL);
    }

    SUBCASE("Float constant") {
        BCConstant c = BCConstant::make_float(3.14159);
        CHECK(c.type == BCConstant::Float);
        CHECK(c.as_float == doctest::Approx(3.14159));
    }

    SUBCASE("String constant") {
        const char* str = "hello";
        BCConstant c = BCConstant::make_string(str, 5);
        CHECK(c.type == BCConstant::String);
        CHECK(c.as_string.data == str);
        CHECK(c.as_string.length == 5);
    }
}

TEST_CASE("BCFunction structure") {
    BCFunction func;
    func.name = StringView("test_func");
    func.param_count = 2;
    func.register_count = 5;

    // Add some code
    func.code.push_back(encode_abi(Opcode::LOAD_INT, 0, 10));
    func.code.push_back(encode_abi(Opcode::LOAD_INT, 1, 20));
    func.code.push_back(encode_abc(Opcode::ADD_I, 2, 0, 1));
    func.code.push_back(encode_abc(Opcode::RET, 2, 0, 0));

    // Add a constant
    func.constants.push_back(BCConstant::make_int(1000000));

    CHECK(func.name == "test_func");
    CHECK(func.param_count == 2);
    CHECK(func.register_count == 5);
    CHECK(func.code.size() == 4);
    CHECK(func.constants.size() == 1);
}

TEST_CASE("BCModule structure") {
    BCModule module;
    module.name = StringView("test_module");

    // Create a function
    BCFunction* func = new BCFunction();
    func->name = StringView("main");
    func->param_count = 0;
    func->register_count = 1;
    func->code.push_back(encode_abi(Opcode::LOAD_INT, 0, 42));
    func->code.push_back(encode_abc(Opcode::RET, 0, 0, 0));

    module.functions.push_back(func);

    CHECK(module.name == "test_module");
    CHECK(module.functions.size() == 1);
    CHECK(module.find_function(StringView("main")) == 0);
    CHECK(module.find_function(StringView("not_found")) == -1);
}

TEST_CASE("Disassemble instruction") {
    Vector<char> out;

    SUBCASE("LOAD_INT") {
        u32 instr = encode_abi(Opcode::LOAD_INT, 0, 42);
        disassemble_instruction(instr, 0, out);
        out.push_back('\0');
        CHECK(strstr(out.data(), "LOAD_INT") != nullptr);
        CHECK(strstr(out.data(), "R0") != nullptr);
        CHECK(strstr(out.data(), "42") != nullptr);
    }

    SUBCASE("ADD_I") {
        out.clear();
        u32 instr = encode_abc(Opcode::ADD_I, 2, 0, 1);
        disassemble_instruction(instr, 1, out);
        out.push_back('\0');
        CHECK(strstr(out.data(), "ADD_I") != nullptr);
        CHECK(strstr(out.data(), "R2") != nullptr);
        CHECK(strstr(out.data(), "R0") != nullptr);
        CHECK(strstr(out.data(), "R1") != nullptr);
    }

    SUBCASE("JMP") {
        out.clear();
        u32 instr = encode_aoff(Opcode::JMP, 0, 5);
        disassemble_instruction(instr, 10, out);
        out.push_back('\0');
        CHECK(strstr(out.data(), "JMP") != nullptr);
        // Jump target should be 10 + 1 + 5 = 16
        CHECK(strstr(out.data(), "16") != nullptr);
    }
}

TEST_CASE("Disassemble function") {
    BCFunction func;
    func.name = StringView("add_two");
    func.param_count = 2;
    func.register_count = 3;

    func.code.push_back(encode_abc(Opcode::ADD_I, 2, 0, 1));
    func.code.push_back(encode_abc(Opcode::RET, 2, 0, 0));

    Vector<char> out;
    disassemble_function(&func, out);
    out.push_back('\0');

    CHECK(strstr(out.data(), "add_two") != nullptr);
    CHECK(strstr(out.data(), "params: 2") != nullptr);
    CHECK(strstr(out.data(), "regs: 3") != nullptr);
    CHECK(strstr(out.data(), "ADD_I") != nullptr);
    CHECK(strstr(out.data(), "RET") != nullptr);
}
