# App Android cho LED Controller

App nối thẳng vào thiết bị qua cổng TCP 7777 (xem `../PROTOCOL.md`) và dùng lại đúng giao diện web của firmware,
nên app và trang web luôn giống nhau. Giao diện được lấy từ `esp8266_led_controller/web_pages.h` lúc build
(`tools/extract_web.py`), sửa giao diện chỉ cần sửa một chỗ.

## Cài đặt

1. Vào tab **Releases** của repo, tải `LedController.apk` ở bản `apk-latest`
   (link cố định: `https://github.com/7th8yuu/arduino-buuld/releases/download/apk-latest/LedController.apk`).
2. Mở file vừa tải, cho phép "cài app từ nguồn không rõ" nếu máy hỏi.
3. Mở app. Điện thoại và thiết bị phải cùng một WiFi.

Cập nhật: cài file mới **đè lên** bản cũ, giữ nguyên cài đặt. Được vì mọi bản đều ký bằng cùng một khoá
(`keystore/ledctl.jks`) và số phiên bản (`versionCode`) luôn tăng theo thời gian build.

## Tự tìm thiết bị

Thử lần lượt, cái nào trả lời `HELLO` đúng giao thức thì nối:

1. IP bạn nhập tay (nếu có), rồi IP của lần nối thành công trước.
2. mDNS dịch vụ `_ledctl._tcp` (Android `NsdManager`).
3. Quét cả dải mạng `x.y.z.1` đến `x.y.z.254` tìm cổng 7777.
4. `192.168.4.1` khi điện thoại đang nối vào WiFi `LED-Setup` của thiết bị.

Mất kết nối thì app tự tìm và nối lại. Khi thoát ra màn hình chính app đóng kết nối để trả chỗ cho thiết bị
(firmware chỉ nhận 2 kết nối TCP cùng lúc). Không thấy thiết bị thì màn hình chờ hiện ô nhập IP.
Nếu firmware đặt `TCP_TOKEN` thì app hỏi mật khẩu đó.

Đổi WiFi cho thiết bị vẫn làm qua trang web của nó (`http://192.168.4.1` khi nối vào `LED-Setup`).
Các nút khởi động lại, ngủ sâu, quên WiFi trong tab Cài đặt hoạt động qua HTTP vì TCP không có các lệnh này.

## Cách hoạt động

- `Finder.kt`: tìm thiết bị (mDNS, quét /24, `HELLO`).
- `Link.kt`: giữ kết nối TCP, `PING` mỗi 25 giây, tự nối lại, nhận sự kiện `SUB 1`.
- `MainActivity.kt`: WebView trỏ vào địa chỉ giả `https://app.ledctl/`, mọi yêu cầu do app tự trả lời (không ra mạng).
- `assets/shim.js`: thay `fetch('/api/...')` của trang web bằng lệnh TCP (`STATE`, `SET`, `APPLY`, `TEST`, `REFRESH`).
- App buộc dùng WiFi cho mọi kết nối để chạy được cả khi WiFi không có Internet.

## Build

CI tự build mỗi lần push thay đổi trong `android/`, `web_pages.h` hoặc `tools/extract_web.py`
(workflow `.github/workflows/build-apk.yml`). APK nằm ở Artifacts của lần chạy và ở Releases.

Build trên máy (cần JDK 17 và Android SDK 34):

```
python3 tools/extract_web.py
cd android
./gradlew assembleRelease
# app/build/outputs/apk/release/app-release.apk
```

## Khoá ký

Repo là open source nên khoá ký để công khai ở `keystore/` (file `ledctl.jks` và mật khẩu trong
`signing.properties`), không dùng GitHub Secrets. Ai build cũng ra APK cùng chữ ký, nên bản mới luôn cài đè được.
Đổi lại, ai cũng có thể tạo APK có chữ ký này, vì vậy chỉ cài APK tải từ repo của bạn.
Đừng dùng khoá này cho app khác. Dấu vân tay SHA-256 của khoá ghi ở `keystore/sha256.txt`,
workflow sẽ báo lỗi nếu APK ký bằng khoá khác. Đổi khoá thì bản cài trước phải gỡ đi cài lại.
