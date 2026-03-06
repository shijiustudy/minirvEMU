#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <am.h>
#include <klib-macros.h>


// 1. 定义硬件状态
uint32_t R[32];      // 32个32位通用寄存器
uint32_t pc = 0;     // 程序计数器
uint32_t M[262144];     // 4KB 内存


// 2. 符号扩展 (Sign Extension)
// 将一个 len 位的数扩展为 32 位有符号数
int32_t sext(uint32_t val, int len) {
    uint32_t sign_bit = 1 << (len - 1);
    // 如果符号位是1，则把高位全部置1
    if (val & sign_bit) {
        return val | (0xFFFFFFFF << len);
    }
    return val; // 否则保持原样
}

// 3. 读写内存辅助函数 (处理大端/小端和对齐)
// 假设你的存储器定义如下

   // 读内存辅助函数
uint32_t mem_read(uint32_t addr, int bytes) {
    uint32_t word_addr = addr >> 2;   // 字节地址转为字索引 (addr / 4)
    uint32_t byte_offset = addr & 0x3; // 计算在 32-bit 字内部的起始字节偏移 (0, 1, 2, 3)
    
    // 从数组中取出一整个 32 位的字
    uint32_t full_word = M[word_addr];

    if (bytes == 4) {
        // 如果是 lw 指令（4字节），通常地址是 4 对齐的，直接返回
        return full_word; 
    } else if (bytes == 1) {
        // 如果是 lbu 指令（1字节），需要根据偏移量抠出对应的 8 位
        // 将目标字节移到最低位，然后屏蔽高位
        return (full_word >> (byte_offset * 8)) & 0xFF;
    }
    return 0;
}

   // 写内存辅助函数
void mem_write(uint32_t addr, uint32_t val, int bytes) {
    uint32_t word_addr = addr >> 2;
    uint32_t byte_offset = addr & 0x3;

    if (bytes == 4) {
        // 如果是 sw 指令，直接覆盖整个字
        M[word_addr] = val;
    } else if (bytes == 1) {
        // 如果是 sb 指令，不能破坏同一个字里的其他 3 个字节
        // 1. 创建掩码，把要写的那 8 位清零（例如偏移为1时，掩码是 0xFFFF00FF）
        uint32_t mask = ~(0xFF << (byte_offset * 8));
        // 2. 将数据放回对应的位置，并写回数组
        M[word_addr] = (M[word_addr] & mask) | ((val & 0xFF) << (byte_offset * 8));
    }
}


// 4. 读文件

void load_bin(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Failed to open binary file");
        exit(1);
    }
    // 将文件读入内存 M，从地址 0 开始存放
    fread(M, 1, sizeof(M), fp);
    fclose(fp);
}


// 5. CPU的执行过程
void cpu_exec(int steps) {
    bool is_running = true;

    while (is_running && steps > 0) {
        // --- Fetch 取指 ---
        // 直接从 uint32_t 数组取出一个字
        uint32_t inst = M[pc >> 2]; 
        
        uint32_t next_pc = pc + 4; // 默认下条指令

        // --- Decode 译码 ---
        uint32_t opcode = inst & 0x7F;
        uint32_t rd     = (inst >> 7) & 0x1F;
        uint32_t funct3 = (inst >> 12) & 0x7;
        uint32_t rs1    = (inst >> 15) & 0x1F;
        uint32_t rs2    = (inst >> 20) & 0x1F;
        uint32_t funct7 = (inst >> 25) & 0x7F;

        // 读取源操作数
        uint32_t src1 = R[rs1];
        uint32_t src2 = R[rs2];
        R[0] = 0; // 确保 x0 恒为 0

        // --- Execute 执行 ---
        switch (opcode) {
            case 0x33: // R-type (add)
                if (funct3 == 0x0 && funct7 == 0x00) {
                    R[rd] = src1 + src2;
                }
                break;

            case 0x13: // I-type (addi)
                if (funct3 == 0x0) {
                    int32_t imm = sext((inst >> 20), 12);
                    R[rd] = src1 + imm;
                }
                break;

            case 0x37: // U-type (lui)
                R[rd] = (inst & 0xFFFFF000);
                break;

            case 0x03: // Load (lw, lbu)
            {
                int32_t imm = sext((inst >> 20), 12);
                uint32_t addr = src1 + imm;
                if (funct3 == 0x2) R[rd] = mem_read(addr, 4);      // lw
                else if (funct3 == 0x4) R[rd] = mem_read(addr, 1); // lbu
                break;
            }

            case 0x23: // Store (sw, sb)
            {
                int32_t imm = sext(((inst >> 25) << 5) | ((inst >> 7) & 0x1f), 12);
                uint32_t addr = src1 + imm;
                if (funct3 == 0x2) mem_write(addr, src2, 4);      // sw
                else if (funct3 == 0x0) mem_write(addr, src2, 1); // sb
                break;
            }

            case 0x67: // jalr
                if (funct3 == 0x0) {
                    int32_t imm = sext((inst >> 20), 12);
                    uint32_t target = (src1 + imm) & ~1;
                    R[rd] = pc + 4;
                    next_pc = target;
                }
                break;

            case 0x73: // System (ebreak)
                if (inst == 0x00100073) { // 严格匹配 ebreak 编码
                    is_running = false; // 停止仿真
                }
                break;

            default:
		printf("Warning!\n"); 
                break;
        }

	printf("PC: 0x%03x | inst: 0x%08x | rd=%2d | val=0x%x\n", pc, inst, rd, R[rd]);

        // 更新 PC
        if (is_running) {
            pc = next_pc;
        }
        R[0] = 0; // Double check
    }

    // ==========================================
    // 程序结束自动判断 (Trap Handling)
    // ==========================================
    // 检查 a0 (x10) 寄存器的值
    if (!is_running) {
        if (R[10] == 0) {
            printf("\033[1;32mHIT GOOD TRAP\033[0m\n"); // 绿色输出
        } else {
            printf("\033[1;31mHIT BAD TRAP\033[0m\n");  // 红色输出
        }
    }
}


int main(int argc, char *argv[]) {
    // 1. 检查用户是否输入了文件名
    if (argc < 2) {
        printf("用法错误！正确格式: %s <文件名.bin>\n", argv[0]);
        return 1;
    }

    // 2. 调用你的 load_bin，传入命令行输入的第一个参数
    // argv[1] 就是你在终端输入的 bin 文件路径
    load_bin(argv[1]);

    // 3. 验证加载（可选，打印前几条指令确认）
    printf("成功加载文件: %s\n", argv[1]);
    printf("首条指令预览: M[0] = 0x%08x\n", M[0]);

    // 4. 注入 ebreak (假设你的程序结束位置在 0x44)
    // 注意：如果不同文件的结束位置不同，你可能需要根据实际情况调整

    printf("开始仿真...\n");
    cpu_exec(10000); // 步数给多一点，确保复杂程序能跑完

    return 0;
}
