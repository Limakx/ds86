# 目录定义
SRC_DIR = src
VENDOR_DIR = vendor\mtcp
BUILD_DIR = build
MTCP_OBJ = $(BUILD_DIR)\mtcpobj
TCP_H_DIR = $(VENDOR_DIR)\TCPINC
TCP_C_DIR = $(VENDOR_DIR)\TCPLIB

# 编译选项
# -bt=dos  目标为16位DOS
# -ms  small内存模型（数据段64K，mTCP要求）
# -ot  速度优先
# -0  8086代码生成
CCOPTS = -bt=dos -ms -ot -0 -DCFG_H="src\DS86.CFG" -i=$(TCP_H_DIR)

# 头文件依赖
DEP_H = $(TCP_H_DIR)\TYPES.H $(TCP_H_DIR)\TIMER.H $(TCP_H_DIR)\TRACE.H &
        $(TCP_H_DIR)\UTILS.H $(TCP_H_DIR)\PACKET.H $(TCP_H_DIR)\ARP.H &
        $(TCP_H_DIR)\TCP.H $(TCP_H_DIR)\TCPSOCKM.H $(TCP_H_DIR)\UDP.H &
        $(TCP_H_DIR)\DNS.H $(TCP_H_DIR)\ETH.H

# 展开成 build\mtcpobj\XXX.OBJ
MTCP_OBJS = $(MTCP_OBJ)\ARP.OBJ $(MTCP_OBJ)\DNS.OBJ $(MTCP_OBJ)\ETH.OBJ &
            $(MTCP_OBJ)\IP.OBJ $(MTCP_OBJ)\PACKET.OBJ $(MTCP_OBJ)\TCP.OBJ &
            $(MTCP_OBJ)\TCPSOCKM.OBJ $(MTCP_OBJ)\TIMER.OBJ &
            $(MTCP_OBJ)\TRACE.OBJ $(MTCP_OBJ)\UDP.OBJ &
            $(MTCP_OBJ)\UTILS.OBJ $(MTCP_OBJ)\IPASM.OBJ

TARGET = $(BUILD_DIR)\DS86.EXE
DS86_OBJ = $(BUILD_DIR)\DS86.OBJ

all : .symbolic $(TARGET)

$(TARGET) : $(DS86_OBJ) $(MTCP_OBJS)
  wlink system dos option map=$(BUILD_DIR)\DS86.MAP option eliminate &
        option stack=4096 name $@ file {$(DS86_OBJ) $(MTCP_OBJS)}

# 主程序规则
$(DS86_OBJ) : $(SRC_DIR)\DS86.CPP $(SRC_DIR)\DS86.CFG $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

# mTCP各模块规则
$(MTCP_OBJ)\ARP.OBJ : $(TCP_C_DIR)\ARP.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\DNS.OBJ : $(TCP_C_DIR)\DNS.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\ETH.OBJ : $(TCP_C_DIR)\ETH.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\IP.OBJ : $(TCP_C_DIR)\IP.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\PACKET.OBJ : $(TCP_C_DIR)\PACKET.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\TCP.OBJ : $(TCP_C_DIR)\TCP.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\TCPSOCKM.OBJ : $(TCP_C_DIR)\TCPSOCKM.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\TIMER.OBJ : $(TCP_C_DIR)\TIMER.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\TRACE.OBJ : $(TCP_C_DIR)\TRACE.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\UDP.OBJ : $(TCP_C_DIR)\UDP.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

$(MTCP_OBJ)\UTILS.OBJ : $(TCP_C_DIR)\UTILS.CPP $(DEP_H)
  wpp $[* -fo=$@ $(CCOPTS)

# 汇编规则
$(MTCP_OBJ)\IPASM.OBJ : $(TCP_C_DIR)\IPASM.ASM
  wasm -bt=dos -ms -0 -i=$(TCP_H_DIR) -fo=$@ $[*

# 清理规则
clean : .symbolic
  @if exist $(BUILD_DIR)\DS86.EXE del $(BUILD_DIR)\DS86.EXE
  @if exist $(BUILD_DIR)\DS86.MAP del $(BUILD_DIR)\DS86.MAP
  @if exist $(DS86_OBJ) del $(DS86_OBJ)
  @if exist $(MTCP_OBJ)\*.OBJ del $(MTCP_OBJ)\*.OBJ
