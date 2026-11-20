#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>

/* mTCP 协议栈 */
#include "types.h"
#include "timer.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "tcp.h"
#include "tcpsockm.h"
#include "udp.h"
#include "dns.h"

/* 版本号 */
#define VERSION "v0.1.0"

/* 屏幕与缓冲区常量 */
#define ATTR_NORMAL 0x07 /* 正常文本属性：黑底白字 */
#define SCR_COLS 80 /* 屏幕列数 */
#define SCR_ROWS 25 /* 屏幕行数 */
#define TEXT_BOTTOM 24 /* 文本区下边界（0-23行）*/

#define BUF_SIZE 1024 /* 用户输入缓冲区大小 */
#define RECV_LINE_SIZE 1460 /* 单行回复缓冲区大小 */
#define XMIT_DATA_SIZE 1024 /* 发送数据区大小 */

/* 历史文件 */
#define HIST_NAME "HISTORY.TXT"
#define HIST_LINE_LEN 80 /* 每行固定记录80字节 */
#define PAGE_LINES TEXT_BOTTOM
#define PAGE_BYTES (PAGE_LINES * HIST_LINE_LEN)
#define CONTENT_LINES (PAGE_LINES - 1) /* 可见内容行数23 */
#define CONTENT_BYTES (CONTENT_LINES * HIST_LINE_LEN) /* 一页内容的字节数 */

/* 配置文件 */
#define CFG_NAME "CONFIG.CFG"

/* 网络 */
#define CONNECT_TIMEOUT_MS 10000L /* 连接建立超时 */
#define NET_TIMEOUT_MS 60000L /* 连接回复超时 */
#define RECV_BUF_SIZE 2048 /* mTCP内部接收缓冲大小 */
#define RECV_CHUNK_SIZE 256 /* 每次recv的块大小 */

/* 全局状态 */
static char proxy_ip[32] = "192.168.1.2";
static int proxy_port = 12345;
static int last_tokens = 0;
static int conn_state = 0; /* 0=未测试 1=已连接 2=未连接 */

static FILE *hist_file = NULL;
static long hist_size = 0;
static long file_pos = 0; /* 屏幕首行在文件中的偏移 */
static int at_bottom = 1; /* 当前是否停在最新位置 */

/* 把光标移到指定位置（行列均从0开始） */
static void cursor_to(int row, int col)
{
    union REGS regs;

    regs.h.ah = 0x02;
    regs.h.bh = 0;
    regs.h.dh = (unsigned char)row;
    regs.h.dl = (unsigned char)col;
    int86(0x10, &regs, &regs);
}

/* 读取当前光标位置 */
static void cursor_get(int *row, int *col)
{
    union REGS regs;

    regs.h.ah = 0x03;
    regs.h.bh = 0;
    int86(0x10, &regs, &regs);

    *row = regs.h.dh;
    *col = regs.h.dl;
}

/* 清屏：调用BIOS终端清屏 */
static void screen_clear(void)
{
    union REGS regs;

    regs.h.ah = 0x06;
    regs.h.al = 0;
    regs.h.bh = ATTR_NORMAL;
    regs.h.ch = 0;
    regs.h.cl = 0;
    regs.h.dh = SCR_ROWS - 1;
    regs.h.dl = SCR_COLS - 1;
    int86(0x10, &regs, &regs);

    cursor_to(0, 0);
}

/* 显示状态栏，保存并恢复原光标位置 */
static void status_draw(void)
{
    int row, col;
    char buf[SCR_COLS + 1];
    const char *state_text;

    if (conn_state == 1)
      state_text = "Connected";
    else
    {
        if (conn_state == 2)
          state_text = "Disconnected";
        else
          state_text = "Unknown";
    }

    sprintf(buf, "Proxy address: %s:%d  %s  Token usage: %d ", proxy_ip, proxy_port, state_text, last_tokens);

    cursor_get(&row, &col);
    cursor_to(SCR_ROWS - 1, 0);
    cprintf("%-79.79s", buf); /* 写满79字符（清除旧内容）*/
    cursor_to(row, col);
}

/* 去掉字符串尾部的空白和换行 */
static void cfg_trim(char *str)
{
    int len;

    len = (int)strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' || str[len - 1] == ' ' || str[len - 1] == '\t'))
    {
        str[--len] = 0;
    }
}

/* 从CONFIG.CFG读取代理地址和端口，成功返回 0 */
static int cfg_load(void)
{
    FILE *fp;
    char line[128];

    fp = fopen(CFG_NAME, "r");
    if (fp == NULL)
      return -1;

    while (fgets(line, sizeof line, fp) != NULL)
    {
        char *equals;
        char *value;

        cfg_trim(line);

        /* 跳过空行和注释行 */
        if (line[0] == 0 || line[0] == ';' || line[0] == '#')
          continue;

        equals = strchr(line, '='); /* 找等号 */
        if (equals == NULL)
          continue;

        *equals = 0; /* 等号前是键 */
        cfg_trim(line);
        value = equals + 1; /*等号后是值*/
        cfg_trim(value);

        if (stricmp(line, "PROXY") == 0 && value[0] != 0)
        {
            strncpy(proxy_ip, value, sizeof proxy_ip - 1);
            proxy_ip[sizeof proxy_ip - 1] = 0;
        }
        else if (stricmp(line, "PORT") == 0)
        {
            int port = atoi(value);

            if (port > 0 && port < 65536)
              proxy_port = port;
        }
    }

    fclose(fp);
    return 0;
}

/* 把当前配置写入CONFIG.CFG，成功返回0 */
static int cfg_save(void)
{
    FILE *fp;

    fp = fopen(CFG_NAME, "w");
    if (fp == NULL)
      return -1;

    fprintf(fp, "PROXY=%s\r\n", proxy_ip);
    fprintf(fp, "PORT=%d\r\n", proxy_port);

    fclose(fp);
    return 0;
}

/* 打开历史文件，优先r+b保留上次内容，文件不存在才新建 */
static int hist_open(void)
{
    long size;

    hist_file = fopen(HIST_NAME, "r+b");
    if (hist_file == NULL)
      hist_file = fopen(HIST_NAME, "w+b");
    if (hist_file == NULL)
      return -1;

    fseek(hist_file, 0L, SEEK_END);
    size = ftell(hist_file);  /* 长度不是80的整数倍说明被外部改过，截到整行边界 */
    size -= size % HIST_LINE_LEN;

    hist_size = size;
    file_pos = 0;
    at_bottom = 1;

    return 0;
}

/* 追加一条物理行记录 */
static void hist_append(const char *text, int text_len)
{
    char record[HIST_LINE_LEN];

    if (text_len > HIST_LINE_LEN - 1)
      text_len = HIST_LINE_LEN - 1;

    memset(record, ' ', HIST_LINE_LEN);
    if (text_len > 0)
      memcpy(record, text, (size_t)text_len);
    record[HIST_LINE_LEN - 1] = (char)text_len;

    if (hist_file == NULL)
      return;

    fseek(hist_file, hist_size, SEEK_SET);
    fwrite(record, 1, HIST_LINE_LEN, hist_file);

    hist_size += HIST_LINE_LEN;
    fflush(hist_file);
}

/* 把字符串按79字符切成多行追加 */
static void hist_append_text(const char *text)
{
    int remain = (int)strlen(text);
    const char *cursor = text;

    if (remain == 0)
    {
        hist_append("", 0);
        return;
    }

    while (remain > 0)
    {
        int chunk;
        if (remain > HIST_LINE_LEN - 1)
          chunk = HIST_LINE_LEN - 1;
        else
          chunk = remain;

        hist_append(cursor, chunk);
        cursor += chunk;
        remain -= chunk;
    }
}

/* 读取指定偏移处的物理行，成功返回0 */
static int hist_read_at(long pos, char *record)
{
    if (hist_file == NULL)
      return -1;
    if (pos < 0 || pos + HIST_LINE_LEN > hist_size)
      return -1;

    fseek(hist_file, pos, SEEK_SET);
    if (fread(record, 1, HIST_LINE_LEN, hist_file) != HIST_LINE_LEN)
      return -1;

    return 0;
}

/* 把一条记录画到指定行 */
static void draw_record(int row, const char *record)
{
    int text_len = (unsigned char)record[HIST_LINE_LEN - 1];
    char buf[HIST_LINE_LEN];

    if (text_len > HIST_LINE_LEN - 1)
      text_len = HIST_LINE_LEN - 1;

    memcpy(buf, record, (size_t)text_len);
    buf[text_len] = 0;

    cursor_to(row, 0);
    cprintf("%-79.79s", buf);
}

/* 从file_pos起重绘文本区（只画0-22行，第23行留给提示符） */
static void redraw_page(void)
{
    char record[HIST_LINE_LEN];
    int row;

    for (row = 0; row < CONTENT_LINES; row++)
    {
        long pos = file_pos + (long)row * HIST_LINE_LEN;

        if (hist_read_at(pos, record) == 0)
          draw_record(row, record);
        else
        {
            cursor_to(row, 0);
            cprintf("%-79.79s", "");
        }
    }
}

/* 把0-last_row整体上滚一行 */
static void scroll_area(int last_row)
{
    union REGS regs;

    regs.h.ah = 0x06;
    regs.h.al = 1;
    regs.h.bh = ATTR_NORMAL;
    regs.h.ch = 0;
    regs.h.cl = 0;
    regs.h.dh = (unsigned char)last_row;
    regs.h.dl = SCR_COLS - 1;
    int86(0x10, &regs, &regs);
}

/* 文本区上滚一行，供历史文件打不开时的降级输出使用 */
static void scroll_text_area(void)
{
    scroll_area(CONTENT_LINES - 1);
}

/* 输入行写满前先滚动，否则会折到第24行把状态栏冲掉 */
static void ensure_input_row(void)
{
    int row, col;

    cursor_get(&row, &col);

    if (row >= CONTENT_LINES && col >= SCR_COLS - 1)
    {
        /* 连输入行一起上滚，再回到空出来的最后一行 */
        scroll_area(CONTENT_LINES);
        cursor_to(CONTENT_LINES, 0);
    }
}

/* 停在最新位置时的光标行号 */
static int hist_cursor_row(void)
{
    long total = hist_size / HIST_LINE_LEN;
    if (total < CONTENT_LINES)
      return (int)total;

    return CONTENT_LINES;
}

/* 把视口移到最新一页，返回光标行号 */
static int hist_bottom_view(void)
{
    long total = hist_size / HIST_LINE_LEN;
    long top = total - CONTENT_LINES;

    if (top < 0)
      top = 0;

    file_pos = top * HIST_LINE_LEN;

    return hist_cursor_row();
}

/* 输出一行文本：写入历史并刷新屏幕 */
static void out_text(const char *text)
{
    int row;

    hist_append_text(text);

    /* 历史文件打不开时降级为直接输出，没有翻页 */
    if (hist_file == NULL)
    {
        int cur_row, cur_col;

        cursor_get(&cur_row, &cur_col);
        if (cur_row >= CONTENT_LINES)
        {
            scroll_text_area();
            cur_row = CONTENT_LINES - 1;
        }

        cursor_to(cur_row, 0);
        cprintf("%.79s\r\n", text);
        return;
    }

    /* 正在翻页看历史时新内容只写文件 */
    if (!at_bottom)
      return;

    row = hist_bottom_view();
    redraw_page();
    cursor_to(row, 0);
}

/* 逐字符读取一行输入，返回长度，PgUp/PgDn翻页，Esc回到底部 */
static int read_line(char *buf, int max_len)
{
    int len = 0;
    int key;
    int row, col;

    buf[0] = 0;

    while (1)
    {
        /* getch()只读一个键且不回显 */
        key = getch();

        /* DOS下键盘对功能键返回两个字节，第一个字节恒为0，第二个字节才是扫描码 */
        if (key == 0)
        {
            /* 必须再读一次，否则扫描码会留在键盘缓冲区里，下次被当作普通字符处理 */
            int scan = getch();

            /* PgUp（扫描码73）：向上翻一页 */
            if (scan == 73)
            {
                /* 钳位，不能翻到文件开头之前 */
                if (file_pos > 0)
                {
                    file_pos -= CONTENT_BYTES;
                    if (file_pos < 0)
                      file_pos = 0;

                    /* 标记已离开底部，此后新输出不再刷新屏幕 */
                    at_bottom = 0;
                    redraw_page();

                    /* 光标放到提示符行，不要压在内容区最后一行 */
                    cursor_to(PAGE_LINES - 1, 0);
                    cprintf("[Paging] Type Esc or Enter to return");
                }
                continue;
            }

            /* PgDn（扫描码81）：向下翻一页 */
            if (scan == 81)
            {
                long max_pos;
                if (hist_size > CONTENT_BYTES)
                  max_pos = hist_size - CONTENT_BYTES;
                else
                  max_pos = 0L;

                if (file_pos < max_pos)
                {
                    file_pos += CONTENT_BYTES;

                    /* 翻到或越过最底部就恢复自动跟随 */
                    if (file_pos >= max_pos)
                    {
                        at_bottom = 1;
                        hist_bottom_view();
                    }

                    redraw_page();

                    if (at_bottom)
                    {
                        cursor_to(hist_cursor_row(), 0);
                        cprintf("> ");
                    }
                    else
                    {
                        cursor_to(PAGE_LINES - 1, 0);
                        cprintf("[Paging] Type Esc or Enter to return");
                    }
                }
                continue;
            }

            /* Esc（扩展键形式）：直接跳回最新位置 */
            if (scan == 27)
            {
                if (!at_bottom)
                {
                    int row;

                    at_bottom = 1;
                    row = hist_bottom_view();
                    redraw_page();
                    cursor_to(row, 0);
                }
                continue;
            }

            continue;
        }

        /* 普通Esc键（非扩展形式） */
        if (key == 27)
        {
            if (!at_bottom)
            {
                int row;

                at_bottom = 1;
                row = hist_bottom_view();
                redraw_page();
                cursor_to(row, 0);
            }
            continue;
        }

        /* 回车：结束输入 */
        if (key == '\r')
        {
            buf[len] = 0;
            return len;
        }

        /* 退格，同时处理跨行回退 */
        if (key == 8)
        {
            if (len > 0)
            {
                len--;
                cursor_get(&row, &col);

                /* 跨行则退到上一行最后一列，用空格擦掉字符 */
                if (col == 0 && row > 0)
                {
                    cursor_to(row - 1, SCR_COLS - 1);
                    cprintf(" ");
                    cursor_to(row - 1, SCR_COLS - 1);
                }
                /* 同行则输出退格 空格 退格，擦掉当前字符 */
                else
                  cprintf("\b \b");
            }
            continue;
        }

        /* 只回显可见字符 */
        if (key >= 32 && key <= 126)
        {
            if (len < max_len - 1)
            {
                buf[len++] = (char)key;

                /* 回显前先确保不会折到状态栏 */
                ensure_input_row();
                cprintf("%c", key);
            }
            /* 缓冲区满则响铃提醒 */
            else
              cprintf("\a");
        }
    }
}

static volatile uint8_t net_abort_flag = 0; /* 必须加volatile，否则可能把!net_abort_flag缓存到寄存器里导致循环永远看不到变化 */

/* Ctrl-Break处理：置中止标志来取消等待网络回复 */
void __interrupt __far net_ctrl_break(void)
{
    net_abort_flag = 1;
}

/* Ctrl-C处理：在数据流里是合法字符，不中止 */
void __interrupt __far net_ctrl_c(void)
{
}

/* mTCP的内存布局约定，data绝不能超过这个长度，否则会写穿到下一个池块，表现为退出时heap is corrupted */
typedef struct
{
    TcpBuffer buf;
    uint8_t data[XMIT_DATA_SIZE];
} xmit_buf_t;

/* 源端口分配，因为每次对话都使用新连接，mTCP会直接使用传入的源端口，传0会导致后续连接冲突 */
static uint16_t next_src_port(void)
{
    static uint16_t port = 0;

    if (port == 0)
      port = (uint16_t)(rand() + 4096); /* mTCP的initStack()内部已经调了srand((unsigned)time(NULL))，不用重复播种 */
    else
    {
        port++;
        if (port > 60000)
          port = 4096;
    }
    return port;
}

/* 处理聊天信息，协议：DOS端发“消息文本\n”，代理端回若干行回复，每行以\n结尾，最后关闭连接，收到连接关闭即视为回复结束，成功返回0 */
static int net_chat(const char *message)
{
    TcpSocket *sock;
    IpAddr_t server_addr;
    static char line_buf[RECV_LINE_SIZE]; /* 栈区可能无法存下，因此开static */
    int line_len = 0;
    int8_t rc; /* 接收mTCP函数返回值（mTCP内部用int8_t） */
    clockTicks_t start_tick; /* 超时计时起点 */
    int got_any = 0; /* 是否收到过任何数据 */
    int msg_len;
    int ip_a, ip_b, ip_c, ip_d; /* 解析 IP 字符串时的临时变量 */

    /* 解析代理IP字符串 */
    if (sscanf(proxy_ip, "%d.%d.%d.%d", &ip_a, &ip_b, &ip_c, &ip_d) != 4)
    {
        out_text("[ERROR] Bad proxy address in CONFIG.CFG");
        return -1;
    }

    server_addr[0] = (uint8_t)ip_a;
    server_addr[1] = (uint8_t)ip_b;
    server_addr[2] = (uint8_t)ip_c;
    server_addr[3] = (uint8_t)ip_d;

    /* 申请socket，池子大小由DS86.CFG里的TCP_MAX_SOCKETS决定 */
    sock = TcpSocketMgr::getSocket();
    if (sock == NULL)
    {
        out_text("[ERROR] No free socket");
        return -1;
    }

    /* 分配接收缓冲区 */
    if (sock->setRecvBuffer(RECV_BUF_SIZE) != 0)
    {
        TcpSocketMgr::freeSocket(sock); /* 失败时先归还socket，否则socket泄漏，下次getSocket()可能返回NULL */
        out_text("[ERROR] Cannot allocate receive buffer");
        return -1;
    }

    /* 连接代理 */
    rc = sock->connect(next_src_port(), server_addr, (uint16_t)proxy_port, CONNECT_TIMEOUT_MS);
    if (rc != 0)
    {
        sock->close(); /* 直接freeSocket会让socket带着未完成的状态被回收，下次复用时状态混乱 */
        TcpSocketMgr::freeSocket(sock);
        conn_state = 2;
        status_draw();
        out_text("[ERROR] Connect failed");
        return -1;
    }
    conn_state = 1;
    status_draw();

    /* 发送消息 */
    msg_len = (int)strlen(message);
    if (msg_len > XMIT_DATA_SIZE - 2) /* 留1字节给\n，1字节余量 */
      msg_len = XMIT_DATA_SIZE - 2;

    /* C89规定变量必须在块首声明，需要用花括号括起来 */
    {
        xmit_buf_t *send_buf = (xmit_buf_t *)TcpBuffer::getXmitBuf(); /* 每次都要重新调用getXmitBuf()，不能加static */

        if (send_buf == NULL)
        {
            sock->close();
            TcpSocketMgr::freeSocket(sock);
            out_text("[ERROR] No transmit buffer");
            return -1;
        }

        memcpy(send_buf->data, message, (size_t)msg_len);
        send_buf->data[msg_len++] = '\n';
        send_buf->buf.setDataLen((uint16_t)msg_len);

        if (sock->enqueue(&send_buf->buf) != 0)
        {
            sock->close();
            TcpSocketMgr::freeSocket(sock);
            out_text("[ERROR] Enqueue failed");
            return -1;
        }
    }

    /* 接收回复 */
    start_tick = TIMER_GET_CURRENT();

    /* mTCP检测到对端发来FIN就置位 */
    while (!sock->isRemoteClosed() && !net_abort_flag)
    {
        uint8_t chunk[RECV_CHUNK_SIZE]; /* 接收数据的临时容器 */
        int16_t got_bytes;
        int i; /* 循环变量 */

        Utils::processPackets(); /* 收到包时，中断会把包放进队列，但需要主程序调用processPackets()才会真正处理 */

        if (Timer_diff(start_tick, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(NET_TIMEOUT_MS))
        {
            out_text("[ERROR] Timeout");
            break;
        }

        got_bytes = sock->recv(chunk, RECV_CHUNK_SIZE);
        if (got_bytes <= 0)
          continue; /* isRemoteClosed()已经在上层处理了关闭的情况 */

        got_any = 1;

        /* 逐字节拼行 */
        for (i = 0; i < got_bytes; i++)
        {
            if (chunk[i] == '\n')
            {
                line_buf[line_len] = 0;

                /* 识别token用量标记，不显示为正文 */
                if (strncmp(line_buf, "[TOKENS ", 8) == 0)
                  last_tokens = atoi(line_buf + 8);
                else
                  out_text(line_buf);

                line_len = 0;
            }
            else
            {
                if (chunk[i] != '\r' && line_len < (int)sizeof(line_buf) - 1)
                  line_buf[line_len++] = (char)chunk[i];
            }
        }
    }

    /* 处理没有换行结尾的残余数据 */
    if (line_len > 0)
    {
        line_buf[line_len] = 0;

        if (strncmp(line_buf, "[TOKENS ", 8) == 0)
          last_tokens = atoi(line_buf + 8);
        else
          out_text(line_buf);
    }

    sock->close();
    TcpSocketMgr::freeSocket(sock);

    if (!got_any)
    {
        out_text("[ERROR] No reply");
        return -1;
    }

    return 0;
}

/* 处理HELP命令 */
static void cmd_help(void)
{
    out_text("Commands:");
    out_text("- HELP             Show this help");
    out_text("- VER              Display version");
    out_text("- SHOW             Display current settings");
    out_text("- SET PROXY <ip>   Set proxy address");
    out_text("- SET PORT <n>     Set proxy port");
    out_text("- QUIT             Exit");
    out_text("");
    out_text("PgUp/PgDn scroll history, and press Esc to jump back to bottom");
}

/* 处理VER命令 */
static void cmd_ver(void)
{
    out_text("DeepSeek for MS-DOS");
    out_text(VERSION);
}

/* 处理SHOW命令 */
static void cmd_show(void)
{
    char buf[sizeof proxy_ip + 16]; /* 格式化输出的中转缓冲区 */

    sprintf(buf, "PROXY=%s", proxy_ip);
    out_text(buf);
    sprintf(buf, "PORT=%d", proxy_port);
    out_text(buf);
}

/* 处理SET命令，若是set命令返回1 */
static int cmd_set(const char *line)
{
    char work[128]; /* line是const，且要原地切分 */
    char *key;
    char *value;
    char buf[128];

    strncpy(work, line, sizeof work - 1); /* 复制一份再处理 */
    work[sizeof work - 1] = 0; /* strncpy不保证补\0，所以下一行手动补 */

    /* 跳过前导空格 */
    key = work;
    while (*key == ' ')
    {
        key++;
    }

    /* 判断是不是SET命令 */
    if (stricmp(key, "SET") != 0 && strnicmp(key, "SET ", 4) != 0)
      return 0;

    /* 取出键名 */
    key += 3;
    while (*key == ' ')
    {
        key++;
    }

    /* 找到值 */
    value = key;
    while (*value != 0 && *value != ' ')
    {
        value++;
    }
    if (*value == 0)
    {
        out_text("Usage: SET PROXY <ip> or SET PORT <n>");
        return 1;
    }

    *value++ = 0;
    while (*value == ' ')
    {
        value++;
    }
    if (*value == 0)
    {
        out_text("Missing value");
        return 1;
    }

    /* 按不同设置项处理 */
    if (stricmp(key, "PROXY") == 0)
    {
        int ip_a, ip_b, ip_c, ip_d;

        if (sscanf(value, "%d.%d.%d.%d", &ip_a, &ip_b, &ip_c, &ip_d) != 4)
        {
            out_text("Bad IP address");
            return 1;
        }

        strncpy(proxy_ip, value, sizeof proxy_ip - 1);
        proxy_ip[sizeof proxy_ip - 1] = 0;
    }
    else
    {
        if (stricmp(key, "PORT") == 0)
        {
            int port = atoi(value);
            if (port <= 0 || port > 65535)
            {
                out_text("Bad port number");
                return 1;
            }

            proxy_port = port;
        }
        else
        {
            out_text("Unknown setting");
            return 1;
        }
    }

    /* 写回配置文件 */
    if (cfg_save() != 0)
    {
        out_text("Cannot write CONFIG.CFG");
        return 1;
    }

    sprintf(buf, "Saved: %s=%s", key, value);
    out_text(buf);
    status_draw();

    return 1;
}

int main(void)
{
    static char input_buf[BUF_SIZE];
    static char echo_buf[BUF_SIZE + 8];
    int hist_ok;

    /* 初始化历史文件 */
    hist_ok = (hist_open() == 0);

    screen_clear();

    out_text("DeepSeek for MS-DOS");

    if (!hist_ok)
      cprintf("Warning: Cannot open %s - scrollback disabled\r\n", HIST_NAME);

    out_text("Starting TCP/IP ...");

    /* 读取mTCP配置 */
    if (Utils::parseEnv() != 0)
    {
        out_text("[ERROR] MTCPCFG not set or bad");
        if (hist_file != NULL)
          fclose(hist_file);
        return 1;
    }

    /* 读取代理配置 */
    if (cfg_load() != 0)
    {
        /* 没有配置文件就把当前默认值落盘，方便用户直接编辑 */
        if (cfg_save() == 0)
          out_text("CONFIG.CFG created with defaults");
        else
          out_text("CONFIG.CFG missing and cannot be created");
    }

    out_text("Initializing stack ...");

    /* 初始化协议栈 */
    {
        int rc = Utils::initStack(2, 2, net_ctrl_break, net_ctrl_c);

        if (rc != 0)
        {
            out_text("[ERROR] InitStack failed");
            if (hist_file != NULL)
              fclose(hist_file);
            return 1;
        }
    }

    screen_clear();
    out_text("Type \"HELP\" for commands, \"QUIT\" to exit");
    out_text("");

    status_draw();

    /* 主循环 */
    while (1)
    {
        if (at_bottom)
        {
            /* 正常模式：提示符紧跟内容，从光标处清到行尾 */
            cursor_to(hist_cursor_row(), 0);
        }
        else
        {
            /* 翻页模式：内容占0-22行，最后一行单独放提示 */
            cursor_to(PAGE_LINES - 1, 0);
        }

        /* 清到行尾：DOS不解析ANSI转义序列，只能自己输出空格 */
        {
            int cur_row, cur_col, n;

            cursor_get(&cur_row, &cur_col);
            for (n = cur_col; n < SCR_COLS; n++)
            {
                cprintf(" ");
            }

            cursor_to(cur_row, cur_col);
        }

        if (at_bottom)
          cprintf("> ");
        else
          cprintf("[Paging] Type Esc or Enter to return");

        /* 空输入则只换行 */
        if (read_line(input_buf, BUF_SIZE) == 0)
        {
            /* 翻页状态下回车表示回到底部 */
            if (!at_bottom)
            {
                int row;

                at_bottom = 1;
                row = hist_bottom_view();
                redraw_page();
                cursor_to(row, 0);
                continue;
            }

            out_text("");
            continue;
        }

        /* 回显用户输入 */
        sprintf(echo_buf, "> %s", input_buf);
        out_text(echo_buf);

        /* 命令分发 */
        if (stricmp(input_buf, "QUIT") == 0)
          break;
        if (stricmp(input_buf, "HELP") == 0)
        {
            cmd_help();
            status_draw();
            continue;
        }
        if (stricmp(input_buf, "VER") == 0)
        {
            cmd_ver();
            status_draw();
            continue;
        }
        if (stricmp(input_buf, "SHOW") == 0)
        {
            cmd_show();
            status_draw();
            continue;
        }
        if (cmd_set(input_buf))
        {
            continue;
        }

        /* 其余输入发给代理 */
        net_chat(input_buf);
        status_draw();
    }

    /* 清理 */
    if (hist_file != NULL)
      fclose(hist_file);
    Utils::endStack();
    screen_clear();

    return 0;
}
