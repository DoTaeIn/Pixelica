"""Each player's first score write fails; the demo must observe and retry it."""
import argparse
import http.client
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import pathlib
import socket
import subprocess
import sys
import threading


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--server",required=True)
    ap.add_argument("--client",required=True)
    ap.add_argument("--launcher",required=True)
    args=ap.parse_args()
    sys.path.insert(0,str(pathlib.Path(args.server).resolve().parent))
    import server as relay
    failed=set()
    guard=threading.Lock()
    with relay.RelayServer(relay.ServerConfig(port=0,db_path=":memory:")) as backend:
        class Proxy(BaseHTTPRequestHandler):
            protocol_version="HTTP/1.1"
            def setup(self):
                super().setup()
                self.connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            def do_GET(self):
                self.answer(b'{"status":"ok"}',"application/json")
            def do_POST(self):
                data=self.rfile.read(int(self.headers["Content-Length"]))
                reject=False
                if len(data)>40 and data[5]==5:
                    with guard:
                        if data[8:40] not in failed:
                            failed.add(data[8:40]);reject=True
                if reject:
                    out=b"P3ON\1\x85\0\x09"
                else:
                    conn=http.client.HTTPConnection("127.0.0.1",backend.port,timeout=3)
                    try:
                        conn.request("POST","/v1/online",body=data,
                                     headers={"Content-Type":"application/octet-stream"})
                        response=conn.getresponse()
                        if response.status!=200: raise RuntimeError("relay HTTP error")
                        out=response.read()
                    finally:
                        conn.close()
                self.answer(out,"application/octet-stream")
            def answer(self,data,ctype):
                self.send_response(200);self.send_header("Content-Length",str(len(data)))
                self.send_header("Content-Type",ctype);self.end_headers();self.wfile.write(data)
            def log_message(self,*unused):
                pass
        proxy=ThreadingHTTPServer(("127.0.0.1",0),Proxy)
        thread=threading.Thread(target=proxy.serve_forever,daemon=True);thread.start()
        try:
            subprocess.run([sys.executable,"-B",args.launcher,"--exe",args.client,
                            "--server-url","http://127.0.0.1:"+str(proxy.server_port),
                            "--mode","versus","--smoke","--timeout","40"],check=True,timeout=45)
            if len(failed)!=2: raise AssertionError("both players must encounter a failed score write")
            print("both score writes retried after asynchronous server failure")
        finally:
            proxy.shutdown();proxy.server_close();thread.join(timeout=2)


if __name__=="__main__":
    main()
