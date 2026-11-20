# Giao thức TCP của LED Controller (phiên bản 1)

Dùng để làm app điều khiển (Android, iOS, PC, Home Assistant...). Web server vẫn chạy bình thường song song,
web và TCP dùng chung một đoạn xử lý lệnh nên cho kết quả giống hệt nhau.

## Kết nối

| | |
|---|---|
| Cổng | `7777` (đổi `TCP_PORT` trong firmware, `0` = tắt TCP) |
| Địa chỉ | IP của thiết bị (hiện ở tab Cài đặt), hoặc `ledctl.local`, hoặc `192.168.4.1` khi nối thẳng vào WiFi `LED-Setup` |
| Tìm tự động | mDNS dịch vụ **`_ledctl._tcp`**, TXT `proto=1`. Android: `NsdManager`, iOS: `NWBrowser` |
| Số kết nối cùng lúc | 2. Kết nối thứ 3 sẽ thay kết nối im lặng lâu nhất (lõi ESP8266 chỉ có 5 kết nối TCP dùng chung cho web và app) |
| Ngắt khi im lặng | 2 phút không nhận được gì. App nên gửi `PING` mỗi 30 giây |
| Mật khẩu | Mặc định không cần. Đặt `TCP_TOKEN "chuoi"` trong firmware thì phải gửi `AUTH chuoi` trước |

Không mã hoá, chỉ dùng trong mạng nhà. Đừng mở cổng này ra Internet.

## Khung dòng

- Mỗi lệnh là **một dòng chữ** kết thúc bằng `\n` (chấp nhận `\r\n`). Không phân biệt hoa thường ở tên lệnh và tên khoá.
- Dạng: `LENH khoa=gia_tri khoa=gia_tri ...` (ngăn cách bằng khoảng trắng, giá trị không chứa khoảng trắng).
- Dòng tối đa 159 ký tự. Dài hơn sẽ nhận `{"ok":0,"err":"too long"}`.
- **Mỗi lệnh có đúng một dòng trả lời, là một đối tượng JSON.** Lỗi luôn có `"ok":0`.
- Dòng trống được bỏ qua.

```
{"ok":0,"err":"unknown"}      lệnh không có
{"ok":0,"err":"auth"}         chưa AUTH hoặc sai mật khẩu
{"ok":0,"err":"too long"}     dòng quá dài
```

## Lệnh

| Lệnh | Trả lời | Việc làm |
|---|---|---|
| `PING` | `{"ok":1,"pong":1}` | Giữ kết nối, đo độ trễ |
| `HELLO` | `{"ok":1,"proto":1,"name":"LED Controller","host":"ledctl","modes":43,"max":500,"auth":0}` | Nhận dạng thiết bị. Dùng được cả khi chưa AUTH |
| `AUTH <mat_khau>` | `{"ok":1}` | Chỉ cần khi firmware có đặt `TCP_TOKEN` |
| `STATE` | đối tượng trạng thái (bên dưới) | Đọc toàn bộ trạng thái |
| `SET khoa=gia_tri ...` | đối tượng trạng thái mới | Đổi một hoặc nhiều thiết lập, tự lưu vào bộ nhớ |
| `APPLY n=146 split=49` | đối tượng trạng thái mới | Đổi số LED và điểm chia hai đoạn cùng lúc |
| `TEST seg=1 c=0 raw=0` | `{"ok":1}` | Thử màu dây ở màn hình căn chỉnh (xem dưới) |
| `REFRESH` | `{"ok":1}` | Gửi lại toàn bộ dữ liệu ra dây LED |
| `SUB 1` / `SUB 0` | `{"ok":1}` | Bật / tắt nhận sự kiện khi trạng thái đổi |

Khoá lạ bị bỏ qua. Giá trị ngoài khoảng được kẹp về khoảng cho phép (ví dụ `bright=999` thành 255).

### Khoá của `SET`

| Khoá | Giá trị | Ý nghĩa |
|---|---|---|
| `on` | `0` hoặc `1` | Tắt / bật đèn. Tắt đèn không làm ESP ngủ, vẫn online |
| `bright` | 1 đến 255 | Độ sáng |
| `mode` | 0 đến 42 | Hiệu ứng (bảng bên dưới) |
| `speed` | 1 đến 255 | Tốc độ hiệu ứng |
| `c1`, `c2` | `RRGGBB` (hex, không có `#`) | Màu 1, màu 2 |
| `o1`, `o2` | 0 đến 5 | Thứ tự màu của dây đoạn 1 / đoạn 2: 0 RGB, 1 RBG, 2 GRB, 3 GBR, 4 BRG, 5 BGR |
| `g1`, `g2` | `RRGGBB` | Căn chỉnh màu từng đoạn, mỗi kênh `ff` = 100% (không chỉnh) |
| `zon` | `0` hoặc `1` | Phát sáng vùng: chỉ sáng từ LED `zs` đến `ze` |
| `zs`, `ze` | 1 đến 500 | LED đầu và cuối của vùng (đếm từ 1) |

`APPLY`: `n` 1 đến 500 (tổng số LED), `split` 1 đến 500 (đoạn 1 gồm LED 1 đến `split`, tự cắt về `n` nếu lớn hơn).

`TEST`: `seg` 0 đến 2 (1 hoặc 2 = đoạn cần thử, 0 = dừng), `c` 0 đỏ, 1 lục, 2 lam, 3 trắng, `255` = dừng,
`raw=1` thử kênh thô, bỏ qua thứ tự màu. Chế độ thử tự dừng sau 10 giây (90 giây khi `raw=1`).

### Đối tượng trạng thái

```json
{"on":1,"n":146,"max":500,"bright":160,"mode":6,"speed":128,"c1":"2de2ff","c2":"0000ff",
 "split":49,"o1":0,"o2":2,"g1":"ffffff","g2":"ffffff","zon":0,"zs":1,"ze":10,
 "rssi":-58,"ip":"192.168.1.40","up":4000,"heap":30000,"rst":"Power On",
 "wd":0,"wr":0,"wa":-1,"hmin":28000,"hblk":24000,"gap":12,"stl":0,"rw":0,"rh":0,"rl":0,"ru":0,"skip":0,"show":0}
```

`rssi` (dBm), `up` (giây từ lúc bật), `heap` (byte trống) và `rst` (lý do khởi động lần cuối) chỉ để hiển thị.

Nhóm chẩn đoán, cũng chỉ để hiển thị (app có thể bỏ qua):

| Khoá | Ý nghĩa |
|---|---|
| `wd` | Số lần WiFi nhà đang nối bị rớt từ lúc bật |
| `wr` | Mã lý do ngắt gần nhất (200 = mất beacon, 201 = không thấy router, 8 = tự rời...) |
| `wa` | Số giây từ lần rớt gần nhất, `-1` nếu chưa rớt |
| `hmin`, `hblk` | Heap trống thấp nhất từ lúc bật, và khối trống liền lớn nhất hiện tại (byte) |
| `gap`, `stl` | Vòng lặp chính bị chặn lâu nhất (ms) và số lần chậm quá 0,5 giây |
| `rw`, `rh`, `rl`, `ru` | ESP đã tự khởi động lại vì mất WiFi `rw` lần, vì cạn bộ nhớ `rh` lần; lần cuối do `rl` (1 WiFi, 2 bộ nhớ) sau `ru` giây chạy. Về 0 khi mất điện |
| `show`, `skip` | Số lần thật sự gửi khung ra dây LED, và số khung giống hệt khung trước nên bỏ qua |
| `rinfo` | Chỉ có khi lần khởi động cuối do treo/lỗi chương trình: chi tiết lỗi |

## Sự kiện

Sau `SUB 1`, mỗi khi trạng thái đổi (do app khác hoặc do trang web), thiết bị gửi thêm một dòng:

```json
{"ev":"state","s":{ ...đối tượng trạng thái... }}
```

- Dòng sự kiện luôn có khoá `ev`, còn dòng trả lời lệnh thì không. Dùng khoá này để phân biệt.
- Người vừa gửi lệnh không nhận lại sự kiện do chính mình gây ra.
- Sự kiện gửi tối đa khoảng 7 lần mỗi giây. Đổi liên tục (kéo thanh trượt) sẽ được gộp lại.
- Các giá trị `rssi`, `up`, `heap` đổi liên tục nhưng **không** kích hoạt sự kiện. Muốn cập nhật thì gửi `STATE`.

## Hiệu ứng (`mode`)

| # | Tên | # | Tên | # | Tên |
|---|---|---|---|---|---|
| 0 | Tĩnh | 15 | Sấm chớp | 30 | Tàu |
| 1 | Thở | 16 | Cảnh sát | 31 | Hoàng hôn |
| 2 | Cầu vồng | 17 | Pháo hoa | 32 | Hai đoạn |
| 3 | Cầu vồng dài | 18 | Cực quang | 33 | Hoa giấy |
| 4 | Đuổi 2 màu | 19 | Cầu vồng sao | 34 | Tung hứng |
| 5 | Sao băng | 20 | Đuổi cầu vồng | 35 | Đèn Noel |
| 6 | Lấp lánh | 21 | Nhịp tim | 36 | Lửa hai đầu |
| 7 | Quét | 22 | Phủ màu | 37 | Lửa màu |
| 8 | Gradient | 23 | Plasma | 38 | Khối màu |
| 9 | Lung linh | 24 | Disco | 39 | Giao thoa |
| 10 | Lửa | 25 | Hai sao băng | 40 | Xi-nhan |
| 11 | Nến | 26 | Đom đóm | 41 | Chẵn lẻ |
| 12 | Sóng | 27 | Bóng nảy | 42 | Sao màu |
| 13 | Mưa | 28 | Đại dương | | |
| 14 | Pha màu | 29 | Lan tỏa | | |

Số hiệu ứng thật của thiết bị là `modes` trong `HELLO`. Hiệu ứng mới luôn được thêm vào cuối, nên số cũ không bao giờ đổi.
App nên so `modes` với bảng của mình, nếu lớn hơn thì hiển thị tên `Hiệu ứng #n` cho các số lạ.

## Ví dụ

Thử nhanh bằng `nc` (gõ lệnh rồi Enter):

```
$ nc ledctl.local 7777
HELLO
{"ok":1,"proto":1,"name":"LED Controller","host":"ledctl","modes":43,"max":500,"auth":0}
SET mode=17 speed=200 bright=255
{"on":1,"n":146,"max":500,"bright":255,"mode":17,"speed":200, ...}
SET c1=ff0000 c2=00ff00
SUB 1
{"ok":1}
```

Python:

```python
import socket, json

s = socket.create_connection(("ledctl.local", 7777), timeout=3)
f = s.makefile("rw", newline="\n")

def cmd(line):
    f.write(line + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "ev" not in r:          # bỏ qua sự kiện, lấy dòng trả lời
            return r

print(cmd("HELLO"))
print(cmd("SET on=1 mode=2 bright=180"))
```

Tìm thiết bị bằng mDNS (Linux): `avahi-browse -rt _ledctl._tcp`.

## Việc chưa có qua TCP

Đổi WiFi, xoá WiFi, khởi động lại và ngủ sâu chỉ có ở web (`/api/forget`, `/api/restart`, `/api/deepsleep`)
vì đây là thao tác có thể làm mất kết nối. Tất cả API web `/api/state`, `/api/set`, `/api/apply`, `/api/test`,
`/api/refresh` vẫn dùng được, cùng khoá và cùng kết quả như lệnh TCP.
