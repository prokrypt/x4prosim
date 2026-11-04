#!/usr/bin/env python3
"""Browser live view of a running x4prosim instance, with touch and keys.

usage: watch.py [NAME] [--port 8404] [--bind 127.0.0.1]
Open http://127.0.0.1:8404/ (from a phone: --bind 0.0.0.0, then the host's LAN address).
The page polls the board state every 300 ms and fetches a new panel picture while a
refresh plays. Press and drag on the picture = finger (swipes work); Up/Down/Power/Home
buttons hold while pressed; keys: arrows, P, H. Without the board state property it
just reloads the picture every poll."""
import argparse, json, os, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x4qmp

PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>x4prosim</title>
<style>
body{margin:0;padding:12px 16px;font:14px system-ui,sans-serif;background:#222;color:#ddd}
#p{display:block;width:min(480px,100%);touch-action:none;user-select:none;background:#888;cursor:crosshair}
#s{margin:8px 0;font-family:ui-monospace,monospace;white-space:pre-wrap;min-height:2.6em}
button{font-size:16px;padding:10px 16px;margin:0 6px 6px 0;touch-action:none}
</style></head><body>
<img id="p" src="panel.png" alt="panel" draggable="false">
<div id="s">connecting...</div>
<div><button data-k="up">Up</button><button data-k="down">Down</button>
<button data-k="power">Power</button><button data-k="home">Home</button></div>
<script>
const p=document.getElementById('p'),s=document.getElementById('s');
let last='',n=0,down=false,lastMove=0;
const send=o=>fetch('input',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});
const pt=e=>{const r=p.getBoundingClientRect();
  return Math.min(479,Math.max(0,Math.round((e.clientX-r.left)*480/r.width)))+','+
         Math.min(799,Math.max(0,Math.round((e.clientY-r.top)*800/r.height)))};
p.onpointerdown=e=>{down=true;p.setPointerCapture(e.pointerId);send({touch:pt(e)})};
p.onpointermove=e=>{if(down&&e.timeStamp-lastMove>40){lastMove=e.timeStamp;send({touch:pt(e)})}};
p.onpointerup=p.onpointercancel=e=>{if(down){down=false;send({touch:'up'})}};
const hold=(k,v)=>send(k=='home'?{home:v}:{key:k,down:v});
for(const b of document.querySelectorAll('button')){const k=b.dataset.k;
  b.onpointerdown=()=>hold(k,true);b.onpointerup=b.onpointerleave=()=>hold(k,false)}
const keys={ArrowUp:'up',ArrowDown:'down',p:'power',h:'home'};
onkeydown=e=>{if(keys[e.key]&&!e.repeat)hold(keys[e.key],true)};
onkeyup=e=>{if(keys[e.key])hold(keys[e.key],false)};
async function poll(){
  try{const r=await fetch('state.json');const j=await r.json();let sig;
    if(j.error){sig=n+1;s.textContent='no board state ('+j.error+'): reloading picture'}
    else{const pn=j.panel,l=j.ledc[0];
      sig=[pn.refreshes,pn.busy,pn.animating,j.sleep].join();
      if(pn.animating)sig+=n;
      s.textContent='t '+(j.uptime_us/1e6).toFixed(1)+' s  '+j.sleep+'  refreshes '+pn.refreshes+
        ' ('+pn.last.wave+', '+pn.last.frames+' frames)'+(pn.busy?'  BUSY':'')+
        '\nlight '+(l[1]?Math.round(l[0]*100/l[1]):0)+'%  battery '+j.battery.soc+'% '+j.battery.mv+' mV'+
        (j.touch.down?'  touch '+j.touch.x+','+j.touch.y:'')}
    if(sig!=last){last=sig;n++;p.src='panel.png?'+n}
  }catch(e){s.textContent='sim not reachable: '+e}
  setTimeout(poll,300)}
poll();
</script></body></html>"""

KEYS = {"up", "down", "power"}


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            self.send(200, PAGE.encode(), "text/html; charset=utf-8")
        elif path == "/state.json":
            try:
                body = json.dumps(x4qmp.state(D))
            except Exception as e:
                body = json.dumps({"error": str(e)})
            self.send(200, body.encode(), "application/json")
        elif path == "/panel.png":
            with SHOT_LOCK:     # last.png is shared with any other reader of the instance dir
                try:
                    x4qmp.shot(D, os.path.join(D, "last.png"))
                    body = open(os.path.join(D, "last.png"), "rb").read()
                except Exception as e:
                    return self.send(503, str(e).encode(), "text/plain")
            self.send(200, body, "image/png")
        else:
            self.send(404, b"not found", "text/plain")

    def do_POST(self):
        # JSON content type only: a cross-site form post can't send it without a preflight we never answer.
        if self.path != "/input" or self.headers.get("Content-Type") != "application/json":
            return self.send(400, b"bad request", "text/plain")
        try:
            o = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
            if "touch" in o:
                t = o["touch"]
                if t != "up":
                    x, y = (int(v) for v in t.split(","))
                    t = f"{x},{y}"
                x4qmp.qom_set(D, "/machine/gt911", "touch", t)
            elif "home" in o:
                x4qmp.qom_set(D, "/machine/gt911", "home", bool(o["home"]))
            elif o.get("key") in KEYS:
                x4qmp.qom_set(D, "/machine/x4pro-keys", o["key"], bool(o["down"]))
            else:
                raise ValueError("unknown input")
        except Exception as e:
            return self.send(400, str(e).encode(), "text/plain")
        self.send(204, b"", "text/plain")

    def log_message(self, *a):
        pass


ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("name", nargs="?", help="instance NAME or dir (default: newest running)")
ap.add_argument("--port", type=int, default=8404)
ap.add_argument("--bind", default="127.0.0.1")
args = ap.parse_args()
D = x4qmp.find(args.name)
SHOT_LOCK = threading.Lock()
print(f"watch: {D} on http://{args.bind}:{args.port}/", file=sys.stderr)
ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()
