# CERTS：VPS 上的 TLS 证书怎么签

设备只连 `https://YOUR_VPS_IP:8446`，并且只信任一个文件：SD 卡根目录的 `shim_ca.pem`。最简单可行的做法是：**签一张自签证书，它既是 VPS 的服务端证书，也是设备 pin 的 CA**。

## 签发（在 VPS 上，或任何有 OpenSSL 的机器上）

```sh
IP=YOUR_VPS_IP
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout key.pem -out cert.pem \
  -subj "/CN=$IP" \
  -addext "subjectAltName=DNS:$IP,IP:$IP" \
  -addext "basicConstraints=critical,CA:TRUE"
chmod 600 key.pem
cp cert.pem shim_ca.pem      # 设备用的就是这张证书本身（只有公钥部分）
```

- `key.pem` 和 `cert.pem` 留在 VPS 上给 socat 用。`key.pem` 不离开 VPS，也不进 git。
- `shim_ca.pem` 拷到 SD 卡根目录，同时放一份到仓库根的 `SD-staging/`（`gateway/install_runtime.py` 会读它）。

### 为什么 SAN 里要把 IP 同时写成 `DNS:` 和 `IP:`

这是真机上踩过的坑。只写 `IP:$IP` 的话，openssl、curl、Python 都能验通，**设备却报 `-9984`（X509 校验失败）**。

原因：设备用的 Arduino-ESP32 2.0.9 带 mbedTLS 2.28。它按裸 IP 连接时，把 IP 字符串当成主机名去比证书。证书有 SAN 时它按规范忽略 CN，但只拿这个字符串去比 `dNSName` 类型的 SAN，不比 `iPAddress` 类型的，于是校验失败。

把 IP 也写成一条 `DNS:` 就能对上。这在 PKIX 上不算规范写法，但 mbedTLS 只做字符串比对。保留 `IP:` 是给其他客户端用的。

### 为什么要 `CA:TRUE`

设备把 `shim_ca.pem` 当成根证书来验服务器。服务器发来的正是这张自签证书，所以它自己必须能当 CA。

## 在 VPS 上起 socat

```sh
socat OPENSSL-LISTEN:8446,reuseaddr,fork,cert=/path/to/cert.pem,key=/path/to/key.pem,verify=0 \
      TCP:127.0.0.1:18124
```

用 systemd 之类的服务管理器让它常驻、开机自启。`verify=0` 指的是不要求设备出示客户端证书；设备身份由网关用 Bearer token 校验。

## 验证

在 VPS 上，等 Mac 的反向隧道连上之后：

```sh
curl --cacert shim_ca.pem https://YOUR_VPS_IP:8446/health
# 期望：{"status": "ok", "service": "stackchan-gateway"}
```

这一步能证明证书、socat、隧道、网关整条链是通的。但证明不了设备那一关，设备那一关只能看串口：连接时不再出现 `-9984` 才算过。

## 换证书

自签证书一换，指纹就变，设备 pin 的那份必须一起换：
1. 在 VPS 上重签，换掉 `cert.pem` / `key.pem`，重启 socat。
2. 把新的 `shim_ca.pem` 拷到 SD 卡根目录和 `SD-staging/`。
3. 插卡开机一次。固件开机时会把卡上的 CA 镜像进内部存储（SPIFFS），之后卡又可以拔掉。
4. 重新跑一次 `gateway/install_runtime.py`（它也会复制这份 CA）。
