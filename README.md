# FULL STORE — PS5 Homebrew Store

Orbit Store tarzı, etaHEN hedefli PS5 ELF payload. `dlpsgame.com/category/ps5/`
üzerinden tüm oyunları (PKG, FPKG, exFAT, DUMP, arşiv) listeler, çoklu-chunk
hızlı indirme yapar, arşivleri açar, PS5'in dahili hafızasına (`/data/etaHEN/games/`
veya seçtiğin yere) kurar.

## Yetenekler

- Katalog tarayıcı: `dlpsgame.com/category/ps5/` — sayfa gezinti, arama,
  izgara görünümü, kapak önbellekleme.
- Oyun detay: her indirme varyantını listeler (format, host, region, versiyon,
  parça numarası).
- Çoklu-host resolver: pixeldrain, buzzheavier, 1fichier (ücretsiz katman),
  mediafire, gofile, direct link.
- Hızlı indirme: HTTP `Range` ile 8 paralel chunk (ayarlanabilir), devam ettirme,
  otomatik hız düşmesi iptali.
- Otomatik ayıklama (zip / 7z / rar / tar.*): libarchive üzerinden akışlı.
- Kurulum:
  - `.pkg` → `/data/etaHEN/games/` (etaHEN FPKG scanner otomatik görür)
  - Decrypted dump / exFAT layout → oyun klasörünü hedef diziline kopyalar
  - USB hedefi için ayrı yol: `/mnt/usb0/PS5/CUSAxxxxx`
- Ayarlar: install path, paralel indirme, chunk sayısı, cookie header
  (Cloudflare için), User-Agent override, etaHEN notify toggle.
- DualSense navigasyonu (ImGui gamepad nav).

## Build

Toolchain: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```bash
# SDK'yı kur
git clone https://github.com/ps5-payload-dev/sdk
cd sdk && make && sudo make install
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

# Gerekli portları kur (SDK'nın port manager'ı üstünden)
pacbrew-pm install curl mbedtls zlib libarchive sdl2 sdl2_image sdl2_ttf freetype

# ImGui'yi vendored olarak projeye düşür
cd /path/to/full-store
mkdir -p third_party && cd third_party
git clone --depth=1 https://github.com/ocornut/imgui
cp imgui/*.cpp imgui/*.h ../src/
cp imgui/backends/imgui_impl_sdl2.{cpp,h} ../src/
cp imgui/backends/imgui_impl_sdlrenderer2.{cpp,h} ../src/
cp imgui/imstb_*.h ../src/

# Derle
cd ..
make
# -> full-store.elf
```

## Çalıştırma

Jailbreak + etaHEN'in çalışır durumda. ELF'i payload loader ile fırlat:

```bash
# BinLoader (varsayılan port 9020) veya etaHEN'in payload loader'ı
nc -q0 <PS5_IP> 9020 < full-store.elf
# ya da Makefile shortcut:
make send IP=192.168.1.42 PORT=9020
```

Uygulama tam ekran açılır. DualSense ile dolaş:
- **D-pad** / sol stick: navigasyon
- **Cross (X)**: tıkla
- **Circle (O)**: geri / popup kapat
- **PS button**: çık

## Dosya düzeni

```
/data/full-store/
  config.json          # ayarlar
  state.json           # tamamlanan iş geçmişi
  full-store.log       # log
  tmp/                 # indirme + ayıklama alanı
  cache/               # http cache
  covers/              # kapak resimleri
```

Varsayılan install path: `/data/etaHEN/games` — etaHEN'in FPKG tarayıcısı burayı
otomatik görür. Ayarlardan değiştir (etaHEN FPFSC, USB0, kendi yolun).

## Site erişim notları

- Cloudflare bir noktada JS challenge çıkarırsa:
  1. Tarayıcıda `dlpsgame.com`'u aç
  2. DevTools → Application → Cookies'den `cf_clearance` değerini kopyala
  3. Settings → Cookie header'a yapıştır: `cf_clearance=...; __cf_bm=...`
- 1fichier premium gerekiyorsa account cookie'sini aynı yere düş.

## Mimari

```
UI (ImGui + SDL2)
      │
      ├─ Scraper      ── fetch_category / fetch_detail (regex HTML parse)
      ├─ Resolver     ── host-specific direct URL çözümleyicileri
      ├─ DL Manager   ── job queue + worker pool, state machine
      │     │
      │     ├─ HTTP   ── libcurl, Range chunk paralel indirme
      │     ├─ Extract── libarchive
      │     └─ Install── route by content (pkg / dump / exfat)
      └─ Settings     ── JSON config
```

Her indirme bir Job: `QUEUED → RESOLVING → DOWNLOADING → EXTRACTING → INSTALLING → COMPLETE`.
Hata olursa `FAILED` state, retry butonu ile tekrar denenir.

## Güvenlik / stabilite notları (bu uygulama için)

- `SSL_VERIFYPEER` kapalı — PS5'teki CA bundle yolları SDK'ya göre değişiyor.
  Kendi CA yolunu `http_client.cpp` içinde `CURLOPT_CAINFO` ile verebilirsin.
- `fork()` kullanmıyor; her şey thread-based (PS5 payload ortamı fork sevmez).
- Downloads dizini için en az indirme boyutu kadar boş yer olmalı; kontrol
  yapılmıyor — kernel yazma hatası alırsan state `FAILED`'e düşer.
- etaHEN notify TCP 9028 varsayılan; etaHEN daemon farklı port kullanıyorsa
  `installer.cpp` içinde değiştir.

## Lisans

Kendi işin. Bu kod temelden homebrew ekosisteminin standart araçlarına
(ps5-payload-sdk, libcurl, libarchive, SDL2, Dear ImGui) dayanır.
