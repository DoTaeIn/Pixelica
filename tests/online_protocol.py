"""Independent malformed-response tests against the native C client parser."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import struct
import subprocess
import threading


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--client",required=True)
    args=parser.parse_args()
    class Handler(BaseHTTPRequestHandler):
        protocol_version="HTTP/1.1"
        def handle_one_request(self):
            try:
                super().handle_one_request()
            except (ConnectionResetError, BrokenPipeError):
                # The native client deliberately abandons rejected replies.
                self.close_connection = True
        def do_POST(self):
            self.rfile.read(int(self.headers["Content-Length"]))
            body=self.server.payload
            self.send_response(self.server.http_status)
            if self.server.http_status==302:
                self.send_header("Location","http://127.0.0.1:9/do-not-follow")
            self.send_header("Content-Type","application/octet-stream")
            self.send_header("Content-Length",str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self,*unused):
            pass
    server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
    server.http_status=200
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    header=b"P3ON\x01\x81\0\0"
    prefix=header+b"t"*32+b"k"*32+b"ABCDEF"+b"\0"
    snapshot=struct.pack(">IBBBBIH",1,0,1,1,8,0,0)+b"\0\4host"+struct.pack(">IB",0,0)
    good=prefix+snapshot
    bad=[good[:i] for i in range(len(good))]
    bad.extend([b"x"*4097,good+b"x",b"bad!"+good[4:],good[:4]+b"\2"+good[5:],
                good[:5]+b"\x82"+good[6:],good[:6]+b"\xff\xff"+good[8:],
                good[:78]+b"\x09"+good[79:],
                prefix+struct.pack(">IBBBBIH",1,0,9,1,8,0,0),
                prefix+struct.pack(">IBBBBIH",1,0,1,3,8,0,0)+snapshot[14:],
                prefix+struct.pack(">IBBBBIH",1,0,1,1,1,0,0)+snapshot[14:],
                prefix+struct.pack(">IBBBBIH",1,0,1,1,8,0,2049)+b"x"*2049+snapshot[14:],
                prefix+struct.pack(">IBBBBIH",1,0,1,1,8,0,0)+b"\0\1\xff"+struct.pack(">IB",0,0),
                prefix+struct.pack(">IBBBBIH",1,1,1,1,8,0,0)+snapshot[14:]])
    try:
        for index,payload in enumerate(bad):
            server.payload=payload
            result=subprocess.run([args.client,"http://127.0.0.1:"+str(server.server_port),
                                   "--expect-status","101"],capture_output=True,text=True,timeout=5)
            if result.returncode:
                raise AssertionError("malformed response %d: %s"%(index,result.stderr))
        for status in range(1,10):
            server.payload=header[:6]+struct.pack(">H",status)
            subprocess.run([args.client,"http://127.0.0.1:"+str(server.server_port),
                            "--expect-status",str(status)],check=True,timeout=5)
        server.http_status=302
        server.payload=b""
        subprocess.run([args.client,"http://127.0.0.1:"+str(server.server_port),
                        "--expect-status","101"],check=True,timeout=5)
        print("native parser rejected %d malformed replies; 9 server errors propagated"%len(bad))
    finally:
        server.shutdown();server.server_close();thread.join(timeout=2)


if __name__=="__main__":
    main()
