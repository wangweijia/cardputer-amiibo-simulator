# Independent synthetic vectors for amiibo_crypto_test.cpp. No private data.
# Format reference: https://github.com/socram8888/amiitool (MIT).
import hashlib,hmac,subprocess
mac=lambda k,d:hmac.new(k,d,hashlib.sha256).digest()
keys=bytearray((i*13+7)&255 for i in range(160))
for offset,name in [(0,b'test data'),(80,b'test tag')]:
 keys[offset+16:offset+30]=bytes(14)
 keys[offset+16:offset+16+len(name)]=name
 keys[offset+31]=16
plain=bytearray((i*7+11)&255 for i in range(520))
plain[0x1d4:0x1dc]=bytes.fromhex('041122bf33445566')
plain[0]=0x44

def derive(master,p):
 seed=p[0x29:0x2b]+bytes(14)+p[0x1d4:0x1dc]*2+p[0x1e8:0x208]
 typ=master[16:30].split(b'\0')[0]+b'\0'
 n=master[31]
 prepared=typ+seed[:16-n]+master[32:32+n]+seed[16:32]+bytes(a^b for a,b in zip(seed[32:],master[48:80]))
 return (mac(master[:16],b'\0\0'+prepared)+mac(master[:16],b'\0\1'+prepared))[:48]

def pack(p):
 p=bytearray(p)
 d=derive(keys[:80],p); t=derive(keys[80:],p)
 p[0x1b4:0x1d4]=mac(t[32:],p[0x1d4:])
 p[8:40]=mac(d[32:],p[0x29:])
 enc=subprocess.run(['openssl','enc','-aes-128-ctr','-K',d[:16].hex(),'-iv',d[16:32].hex()],input=p[0x2c:0x1b4],capture_output=True,check=True).stdout
 p[0x2c:0x1b4]=enc
 out=bytearray(540)
 for dst,src,size in [(8,0,8),(0x80,8,0x20),(0x10,0x28,0x24),(0xa0,0x4c,0x168),(0x34,0x1b4,0x20),(0,0x1d4,8),(0x54,0x1dc,0x2c)]: out[dst:dst+size]=p[src:src+size]
 for i in range(520,540):out[i]=(i*3)&255
 return out
out=pack(plain)
print('pack_sha256',hashlib.sha256(out).hexdigest())
uid=bytes.fromhex('04a1b2c3d4e5f6')
plain[0x1d4:0x1dc]=uid[:3]+bytes([0x88^uid[0]^uid[1]^uid[2]])+uid[3:]
plain[0]=uid[3]^uid[4]^uid[5]^uid[6]
out=pack(plain)
out[532:538]=bytes([0xaa^uid[1]^uid[3],0x55^uid[2]^uid[4],0xaa^uid[3]^uid[5],0x55^uid[4]^uid[6],0x80,0x80])
print('rebind_sha256',hashlib.sha256(out).hexdigest())
