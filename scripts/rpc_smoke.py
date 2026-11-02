#!/usr/bin/env python3
"""Nightfall 核心的 RPC 冒烟测试。

启动 player-core.exe，用和 Electron 主进程相同的 Content-Length 帧协议
依次验证：扫描、搜索、根目录/专辑过滤、播放列表 CRUD/重排/重命名、
播放/暂停/跳转/下一首/自动切歌、音量、队列、移除/重新添加根目录。

用法（默认 null 输出，不发声）：
    python -I scripts/rpc_smoke.py

走真实 WASAPI 但不发声：
    NIGHTFALL_AUDIO=wasapi NIGHTFALL_TEST_VOLUME=0 python -I scripts/rpc_smoke.py

只使用 player-core/runtime 里的 DLL（验证自包含打包）：
    NIGHTFALL_MSYS2_BIN=I:\\SE\\player-core\\runtime python -I scripts/rpc_smoke.py
"""

import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time

# 仓库根由脚本位置推导，避免硬编码盘符路径。
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "player-core", "build", "player-core.exe")
MEDIA = os.path.join(ROOT, "test-media")
# 临时中文目录，用于回归测试“移除中文路径根目录”。
CHINESE_ROOT = os.path.join(ROOT, "test-media-中文")
DB = os.path.join(ROOT, "player-core", "build", "smoke.db")

# 核心的 DLL 搜索路径。默认用 MSYS2；可以用 runtime 目录验证自包含打包。
MSYS2_BIN = os.environ.get("NIGHTFALL_MSYS2_BIN", r"I:\msys64\mingw64\bin")


class Client:
    """最小 RPC 客户端：启动核心、发请求、收通知。

    stdout 由后台线程按 Content-Length 解析：
      * 有 id -> 放入 responses，供 request() 等待
      * 无 id -> 通知，放入 notifications 队列
    stderr 单独收集，测试失败时打印最后几行帮助排错。
    """

    def __init__(self, exe, db):
        # 子进程 PATH 前置 DLL 目录；音频模式默认 null（测试不发声）。
        env = os.environ.copy()
        env = os.environ.copy()
        env["PATH"] = MSYS2_BIN + os.pathsep + env.get("PATH", "")
        self.audio_mode = os.environ.get("NIGHTFALL_AUDIO", "null")
        self.proc = subprocess.Popen(
            [exe, "--db", db, "--audio", self.audio_mode],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            bufsize=0,
        )
        self.responses = {}
        self.notifications = queue.Queue()
        self.stderr_lines = []
        self.lock = threading.Lock()
        self.next_id = 1
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        # 读取 LSP 风格帧：头部 -> 空行 -> body（UTF-8 JSON）。
        stream = self.proc.stdout
        while True:
            headers = {}
            while True:
                line = stream.readline()
                if not line:
                    return
                if line in (b"\r\n", b"\n"):
                    break
                name, _, value = line.decode("ascii", "replace").partition(":")
                headers[name.strip().lower()] = value.strip()
            length = int(headers.get("content-length", "0"))
            body = stream.read(length)
            if len(body) != length:
                return
            message = json.loads(body.decode("utf-8"))
            if "id" in message:
                with self.lock:
                    self.responses[message["id"]] = message
            else:
                self.notifications.put(message)

    def _read_stderr(self):
        # 核心日志走 stderr，收集起来在失败时展示。
        for line in self.proc.stderr:
            self.stderr_lines.append(line.decode("utf-8", "replace").rstrip())

    def request(self, method, params=None, timeout=10.0):
        # 分配 id -> 写帧 -> 轮询 responses（后台线程会填充）。
        with self.lock:
            request_id = self.next_id
            self.next_id += 1
        payload = json.dumps(
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
        ).encode("utf-8")
        frame = b"Content-Length: %d\r\n\r\n" % len(payload) + payload
        self.proc.stdin.write(frame)
        self.proc.stdin.flush()

        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                message = self.responses.pop(request_id, None)
            if message is not None:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message["result"]
            time.sleep(0.02)
        raise TimeoutError(method)

    def wait_notification(self, method, timeout=10.0):
        # 等待指定通知；期间收到的其他通知会被丢弃（本测试不依赖它们）。
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.notifications.get(timeout=0.2)
            except queue.Empty:
                continue
            if message.get("method") == method:
                return message.get("params")
        return None

    def close(self):
        # 先优雅关闭；超时/异常则直接 terminate，最后 kill 兜底。
        try:
            self.request("system.shutdown", timeout=1)
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


def main():
    # 每次从干净数据库开始，避免历史播放列表/根目录影响断言。
    for path in (CORE, MEDIA):
        if not os.path.exists(path):
            raise SystemExit(f"missing: {path}")
    if os.path.exists(DB):
        os.remove(DB)
    shutil.rmtree(CHINESE_ROOT, ignore_errors=True)

    client = Client(CORE, DB)
    try:
        # ---- 基本连通性 ----
        assert client.request("ping")["ok"] is True
        assert client.request("library.roots")["roots"] == []

        # 添加根目录会启动异步扫描；等 library.changed 再查询。
        client.request("library.addRoot", {"path": MEDIA})
        changed = client.wait_notification("library.changed", timeout=20)
        assert changed is not None, "scan did not finish"

        page = client.request("library.page", {"offset": 0, "limit": 100})
        print("tracks:", page["total"])
        assert page["total"] == 4, page["total"]
        titles = sorted(t["title"] for t in page["tracks"])
        print("titles:", titles)
        assert "Test Tone A" in titles, titles

        # ---- 搜索 / 根目录过滤 ----
        search = client.request(
            "library.page", {"offset": 0, "limit": 100, "query": "Tone A"}
        )
        print("search total:", search["total"])
        assert search["total"] >= 1

        filtered = client.request(
            "library.page", {"offset": 0, "limit": 100, "root": MEDIA}
        )
        print("root filter total:", filtered["total"])
        assert filtered["total"] == 4
        outside = client.request(
            "library.page",
            {"offset": 0, "limit": 100, "root": os.path.join(MEDIA, "missing")},
        )
        assert outside["total"] == 0

        # ---- 专辑聚合与按专辑过滤 ----
        albums = client.request("library.albums")["albums"]
        print("albums:", albums)
        assert any(
            album["album"] == "Smoke Tests" and album["trackCount"] == 4
            for album in albums
        )
        album_page = client.request(
            "library.page", {"offset": 0, "limit": 100, "album": "Smoke Tests"}
        )
        assert album_page["total"] == 4
        missing_album = client.request(
            "library.page", {"offset": 0, "limit": 100, "album": "Missing Album"}
        )
        assert missing_album["total"] == 0

        track = next(t for t in page["tracks"] if t["title"] == "Test Tone A")
        assert client.request("library.track", {"id": track["id"]})["title"] == "Test Tone A"
        ids = [t["id"] for t in page["tracks"]]

        # 新建列表 -> 添加 3 首 -> 重排 -> 重命名 -> 移除/删除。
        assert client.request("playlists.list")["playlists"] == []
        created = client.request("playlists.create", {"name": "Road Trip"})
        playlist_id = created["id"]
        client.request(
            "playlists.addTrack",
            {"playlistId": playlist_id, "trackId": ids[0]},
        )
        client.request(
            "playlists.addTrack",
            {"playlistId": playlist_id, "trackId": ids[1]},
        )
        client.request(
            "playlists.addTrack",
            {"playlistId": playlist_id, "trackId": ids[2]},
        )
        playlist = client.request("playlists.tracks", {"id": playlist_id})
        print("playlist:", playlist["playlist"], [t["title"] for t in playlist["tracks"]])
        assert playlist["playlist"]["trackCount"] == 3
        assert len(playlist["tracks"]) == 3

        client.request(
            "playlists.reorder",
            {"id": playlist_id, "trackIds": [ids[2], ids[0], ids[1]]},
        )
        playlist = client.request("playlists.tracks", {"id": playlist_id})
        titles = [t["title"] for t in playlist["tracks"]]
        print("reordered:", titles)
        assert titles == ["Test Tone C", "Test Tone A", "Test Tone B"], titles

        client.request(
            "playlists.rename",
            {"id": playlist_id, "name": "Night Mix"},
        )
        listed = client.request("playlists.list")["playlists"]
        assert any(p["id"] == playlist_id and p["name"] == "Night Mix" for p in listed)

        client.request(
            "playlists.removeTrack",
            {"playlistId": playlist_id, "trackId": ids[0]},
        )
        playlist = client.request("playlists.tracks", {"id": playlist_id})
        assert len(playlist["tracks"]) == 2
        assert playlist["playlist"]["trackCount"] == 2
        client.request("playlists.delete", {"id": playlist_id})
        assert client.request("playlists.list")["playlists"] == []
        # ---- 播放：播放/暂停/跳转/下一首/自动切歌 ----
        client.request("queue.set", {"ids": ids})
        client.request(
            "playback.setVolume",
            {"volume": float(os.environ.get("NIGHTFALL_TEST_VOLUME", "0.8"))},
        )
        client.request("playback.play", {"trackId": track["id"]})

        time.sleep(1.2)
        state = client.request("playback.state")
        print("playing state:", state)
        assert state["playing"] is True
        assert state["positionMs"] > 300, state
        assert state["audioMode"] == client.audio_mode, state

        client.request("playback.pause")
        p1 = client.request("playback.state")["positionMs"]
        time.sleep(0.6)
        p2 = client.request("playback.state")["positionMs"]
        print("paused position:", p1, p2)
        assert abs(p2 - p1) < 150, (p1, p2)

        client.request("playback.seek", {"positionMs": 2000})
        client.request("playback.resume")
        time.sleep(0.8)
        state = client.request("playback.state")
        print("after seek:", state)
        assert state["positionMs"] >= 2000, state

        before = state["trackId"]
        client.request("playback.next")
        time.sleep(0.5)
        after = client.request("playback.state")
        print("after next:", after)
        assert after["trackId"] != before

        client.request("playback.seek", {"positionMs": 3600})
        time.sleep(2.0)
        advanced = client.request("playback.state")
        print("auto-advance:", advanced)
        assert advanced["trackId"] == 3, advanced

        client.request("playback.setVolume", {"volume": 0.3})
        assert abs(client.request("playback.state")["volume"] - 0.3) < 1e-6

        queue_state = client.request("queue.get")
        assert len(queue_state["ids"]) == 4

        # ---- 移除根目录并重新添加，验证清理与恢复 ----
        client.request("library.removeRoot", {"path": MEDIA})
        assert client.request("library.roots")["roots"] == []
        assert client.request("library.page", {"offset": 0, "limit": 100})["total"] == 0

        client.request("library.addRoot", {"path": MEDIA})
        deadline = time.time() + 15
        restored = 0
        while time.time() < deadline:
            restored = client.request("library.page", {"offset": 0, "limit": 100})["total"]
            if restored == 4:
                break
            time.sleep(0.2)
        print("restored after remove/re-add:", restored)
        assert restored == 4

        # ---- 中文路径根目录移除（回归：substr 字节/字符长度 bug）----
        # 旧实现用 substr(path,1,字节数)，中文路径下条件不成立，曲目会残留。
        os.makedirs(CHINESE_ROOT, exist_ok=True)
        shutil.copyfile(
            os.path.join(MEDIA, "01-test-a.flac"),
            os.path.join(CHINESE_ROOT, "chinese-test.flac"),
        )
        client.request("library.addRoot", {"path": CHINESE_ROOT})
        deadline = time.time() + 15
        chinese_total = 0
        while time.time() < deadline:
            chinese_total = client.request(
                "library.page",
                {"offset": 0, "limit": 100, "root": CHINESE_ROOT},
            )["total"]
            if chinese_total == 1:
                break
            time.sleep(0.2)
        assert chinese_total == 1, chinese_total

        client.request("library.removeRoot", {"path": CHINESE_ROOT})
        remaining = client.request(
            "library.page",
            {"offset": 0, "limit": 100, "root": CHINESE_ROOT},
        )["total"]
        print("tracks left after removing Chinese root:", remaining)
        assert remaining == 0, remaining
        assert client.request("library.page", {"offset": 0, "limit": 100})["total"] == 4

        print("SMOKE OK")
    finally:
        client.close()
        shutil.rmtree(CHINESE_ROOT, ignore_errors=True)
        if client.stderr_lines:
            print("--- core stderr ---")
            print("\n".join(client.stderr_lines[-20:]))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("SMOKE FAILED:", exc)
        sys.exit(1)
