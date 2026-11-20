import socket
import json
import os
import time
import urllib.request
import traceback

HOST = "0.0.0.0"
PORT = 12345

API_URL = "https://api.deepseek.com/chat/completions"

# 配置文件固定放在脚本同级目录
CONFIG_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "proxy_config.json"
)

# 模型提示词
SYSTEM_PROMPT = (
    "You are answering a user on an MS-DOS client."
    "Respond in English only."
    "Use only printable ASCII (32-126), no emoji/smart quotes/em-dashes/ellipsis/Markdown."
    "Use '-' '\"' '...' instead"
)

# 完整的对话上下文
history = []

# 从proxy_config.json读取配置，缺少api_key时直接退出
def load_config():
    if not os.path.exists(CONFIG_FILE):
        raise SystemExit(
            "Config file not found: %s\n"
            "Create it with:\n"
            "{\n"
            '    "api_key": "sk-xxx",\n'
            '    "model": "deepseek-chat"\n'
            "}" % CONFIG_FILE
        )

    try:
        with open(CONFIG_FILE, "r", encoding="UTF-8") as f:
            cfg = json.load(f)
    except (OSError, ValueError) as e:
        raise SystemExit("Cannot read %s: %s" % (CONFIG_FILE, e))

    if not cfg.get("api_key"):
        raise SystemExit('%s has no "api_key"' % CONFIG_FILE)

    return cfg

# 按统一格式输出一行日志
def log(tag, msg):
    print("[%s] %s %s" % (time.strftime("%H:%M:%S"), tag, msg))

# 把用户输入连同历史发给模型
def call_deepseek(user_text, cfg):
    log("API", "Sending user text (%d chars): %r" % (len(user_text), user_text))

    history.append({"role": "user", "content": user_text})

    payload = {
        "model": cfg.get("model", "deepseek-chat"),
        "messages": [{"role": "system", "content": SYSTEM_PROMPT}] + history,
        "stream": True,
    }

    log("API", "History length now %d messages" % len(history))

    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("UTF-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + cfg["api_key"],
        },
        method="POST",
    )

    parts = []
    chunk_count = 0
    total_tokens = 0
    start_time = time.time()

    # 逐行迭代响应体，服务端产生一段就吐一行
    with urllib.request.urlopen(req, timeout=120) as resp:
        log("API", "HTTP status: %d" % resp.status)

        for raw in resp:
            line = raw.decode("UTF-8", "replace").strip()

            if not line.startswith("data:"):
                continue

            data = line[5:].strip()

            if data == "[DONE]":
                log("API", "Stream done marker received")
                break

            try:
                obj = json.loads(data)
            except json.JSONDecodeError as e:
                # 单个数据块解析失败就跳过，不影响整个流
                log("API", "JSON decode error: %s" % e)
                continue

            # usage只在最后一个数据块里出现，有就记下来
            usage = obj.get("usage")
            if usage:
                total_tokens = usage.get("total_tokens", 0)
                log(
                    "API",
                    "usage: prompt=%s completion=%s total=%s"
                    % (
                        usage.get("prompt_tokens"),
                        usage.get("completion_tokens"),
                        usage.get("total_tokens"),
                    ),
                )

            # 避免choices为空时下标越界
            delta = obj.get("choices", [{}])[0].get("delta", {})
            piece = delta.get("content")
            if piece:
                chunk_count += 1
                parts.append(piece)

    elapsed = time.time() - start_time
    reply = "".join(parts)

    log(
        "API",
        "Received %d chunks in %.2fs, reply len %d"
        % (chunk_count, elapsed, len(reply)),
    )
    log("API", "Raw reply: %r" % reply)

    # 换行会破坏一行一条消息的协议，统一压成空格
    reply_clean = reply.replace("\r", " ").replace("\n", " ")
    if reply_clean != reply:
        log("API", "Newlines replaced with spaces")

    history.append({"role": "assistant", "content": reply_clean})
    return reply_clean, total_tokens

# 从连接读一行（以换行结尾），对端关闭时返回None
def recv_line(conn):
    buf = b""

    while True:
        data = conn.recv(1024)

        if not data:
            log("RECV", "Connection closed by client, buf=%r" % buf)
            return None

        buf += data
        log("RECV", "Got %d bytes, total %d" % (len(data), len(buf)))

        if b"\n" in buf:
            line, _rest = buf.split(b"\n", 1)
            return line.decode("UTF-8", "replace")

    return None

# 处理客户端连接
def handle(conn, addr, cfg):
    log("CONN", "Client connected from %s:%d" % addr)

    text = recv_line(conn)

    if text is None:
        conn.close()
        return

    text = text.strip()
    log("CONN", "Parsed request: %r" % text)

    # 空请求不调模型，直接回一行提示
    if not text:
        log("CONN", "Empty request, sending error")
        conn.sendall(b"empty message\n")
        conn.close()
        return

    try:
        reply, tokens = call_deepseek(text, cfg)
    except Exception as e:
        # 任何异常都转成一行错误信息回给客户端，不让连接静默断掉
        log("ERR", "API call failed: %s" % e)
        traceback.print_exc()
        reply = "Proxy error: " + str(e)[:60]
        tokens = 0

    # 最后一行固定是 token 标记，DOS端据此更新状态栏
    lines = reply.split("\n")
    lines.append("[TOKENS %d]" % tokens)
    log("SEND", "Sending %d line(s) to client (tokens=%d)" % (len(lines), tokens))

    try:
        for i, chunk in enumerate(lines):
            payload = chunk.encode("UTF-8") + b"\n"
            log("SEND", "Line %d: %r" % (i, chunk))
            conn.sendall(payload)

        log("SEND", "All lines sent, closing")
    except Exception as e:
        log("ERR", "Send failed: %s" % e)

    # 关闭连接即表示回复结束
    conn.close()

def main():
    cfg = load_config()

    log("START", "Proxy starting")
    log("START", "API key loaded (%d chars)" % len(cfg["api_key"]))
    log("START", "Model: %s" % cfg.get("model", "deepseek-chat"))
    log("START", "Listening on %s:%d" % (HOST, PORT))

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    # 若不加这项重启时旧连接还在TIME_WAIT会bind失败
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    s.bind((HOST, PORT))
    s.listen(4)

    try:
        while True:
            conn, addr = s.accept()

            try:
                handle(conn, addr, cfg)
            except Exception as e:
                log("ERR", "Handler crashed: %s" % e)
                traceback.print_exc()

                try:
                    conn.close()
                except Exception:
                    pass

    except KeyboardInterrupt:
        log("STOP", "Interrupted by user")
    finally:
        s.close()

if __name__ == "__main__":
    main()
