"""Isolated relay + native C client integration; no internet, no third-party Python."""
import argparse
import http.client
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--openssl")
    args = parser.parse_args()
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="pixelica-online-") as directory:
        logpath = pathlib.Path(directory) / "server.log"
        with logpath.open("w+") as log:
            import sys
            proc = subprocess.Popen([sys.executable, "-B", args.server, "--host", "127.0.0.1",
                                     "--port", str(port), "--db", str(pathlib.Path(directory)/"scores.db")],
                                    stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if proc.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    try:
                        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=.2)
                        conn.request("GET", "/health")
                        if conn.getresponse().status == 200:
                            conn.close()
                            break
                    except OSError:
                        time.sleep(.05)
                else:
                    raise RuntimeError("relay did not become ready")
                subprocess.run([args.client, "http://127.0.0.1:"+str(port)], check=True, timeout=75)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
        if args.openssl:
            cert = pathlib.Path(directory)/"cert.pem"
            key = pathlib.Path(directory)/"key.pem"
            subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-days", "1", "-subj", "/CN=localhost", "-keyout", str(key),
                            "-out", str(cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(str(cert),str(key))
            with socket.socket() as listener:
                listener.bind(("127.0.0.1",0));listener.listen(1);listener.settimeout(3)
                tlsport=listener.getsockname()[1]
                def accept():
                    try:
                        raw,_=listener.accept()
                        with raw:
                            try:
                                with ctx.wrap_socket(raw,server_side=True) as tls:
                                    tls.recv(4096)
                            except (OSError,ssl.SSLError):
                                pass
                    except OSError:
                        pass
                thread=threading.Thread(target=accept,daemon=True);thread.start()
                subprocess.run([args.client,"https://localhost:"+str(tlsport),"--reject-tls"],check=True,timeout=10)
                thread.join(timeout=4)


if __name__=="__main__":
    main()
