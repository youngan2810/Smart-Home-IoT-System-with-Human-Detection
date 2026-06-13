# Smart Home IoT System with Human Detection

## Giới thiệu

Dự án xây dựng một hệ thống nhà thông minh kết hợp **IoT** và **thị giác máy tính** nhằm giám sát sự hiện diện của con người, điều khiển thiết bị điện và hỗ trợ tiết kiệm năng lượng. Hệ thống sử dụng **ESP32-CAM** để thu hình ảnh, **AI Server chạy Python** để phát hiện người bằng **YOLOv3** kết hợp nhận diện khuôn mặt bằng **Haar Cascade**, sau đó gửi trạng thái phát hiện qua **MQTT/HiveMQ Cloud** đến **NodeMCU ESP8266** để điều khiển đèn, quạt và cập nhật dữ liệu lên **Blynk**.

Ngoài chức năng phát hiện người, hệ thống còn tích hợp cảm biến **DHT11** để đo nhiệt độ, độ ẩm và cảm biến **MQ2** để giám sát khí gas/khói. Người dùng có thể theo dõi trạng thái hệ thống, nhận cảnh báo và điều khiển thiết bị từ xa thông qua ứng dụng Blynk.

## Tính năng chính

- Phát hiện sự hiện diện của người bằng YOLOv3.
- Hỗ trợ phát hiện khuôn mặt bằng Haar Cascade trong trường hợp camera chỉ thấy một phần cơ thể.
- Duy trì trạng thái phát hiện trong một khoảng thời gian ngắn để tránh bật/tắt thiết bị liên tục.
- Truyền trạng thái phát hiện người từ AI Server đến ESP8266 qua MQTT.
- Điều khiển đèn và quạt tự động dựa trên trạng thái có người/không có người.
- Cho phép điều khiển thủ công thông qua Blynk.
- Giám sát nhiệt độ, độ ẩm và khí gas/khói bằng DHT11 và MQ2.
- Gửi cảnh báo qua Blynk khi phát hiện khí gas/khói bất thường hoặc thiết bị bị bỏ bật khi không có người.

## Kiến trúc hệ thống

Luồng hoạt động tổng quát:

```text
ESP32-CAM
   ↓ HTTP/MJPEG Stream
Local PC / AI Server
   ↓ MQTT Publish
HiveMQ Cloud
   ↓ MQTT Subscribe
NodeMCU ESP8266
   ↓
Relay / IR Module / Sensors
   ↓
Blynk Cloud + Mobile App
```

Các thành phần chính:

| Thành phần | Vai trò |
|---|---|
| ESP32-CAM | Thu nhận hình ảnh và truyền stream về AI Server |
| Local PC / AI Server | Chạy Python, YOLOv3 và Haar Cascade để phát hiện người |
| HiveMQ Cloud | MQTT Broker trung gian giữa AI Server và ESP8266 |
| NodeMCU ESP8266 | Gateway điều khiển thiết bị, đọc cảm biến và kết nối Blynk |
| Blynk | Giao diện giám sát, điều khiển và cảnh báo trên điện thoại |
| DHT11 | Đo nhiệt độ và độ ẩm |
| MQ2 | Giám sát khí gas/khói |
| Relay / IR Module | Điều khiển đèn, quạt hoặc thiết bị ngoại vi |

## Công nghệ sử dụng

### Phần cứng

- ESP32-CAM
- NodeMCU ESP8266
- Cảm biến DHT11
- Cảm biến MQ2
- Relay module
- IR Receiver / IR Transmitter
- Đèn LED, quạt DC hoặc thiết bị mô phỏng tải

### Phần mềm và nền tảng

- Python
- OpenCV
- YOLOv3
- Haar Cascade
- MQTT
- HiveMQ Cloud
- Blynk IoT
- Arduino IDE
- ESP8266/ESP32 libraries

## Cấu trúc thư mục tham khảo

```text
Smart-Home-IoT-System-with-Human-Detection/
├── AI detect/
│   ├── ai_human_mqtt.py
│   ├── coco.names
│   ├── yolov3.cfg
│   ├── haarcascade_frontalface_default.xml
│   └── yolov3.weights        # Không upload lên GitHub do file quá lớn
│
├── Code/
│   └── smart_home.ino        # Code cho NodeMCU ESP8266
│
├── ESP32 CAM/
│   └── esp32cam.ino          # Code cho ESP32-CAM
│
├── README.md
└── .gitignore
```

## Lưu ý về file YOLOv3 weights

File `yolov3.weights` có dung lượng lớn hơn giới hạn upload trực tiếp của GitHub, vì vậy file này không nên commit vào repository.

Sau khi clone project, cần tải file `yolov3.weights` riêng và đặt vào thư mục:

```text
AI detect/yolov3.weights
```

Nên thêm vào `.gitignore`:

```gitignore
*.weights
AI detect/yolov3.weights
```

## Cách chạy hệ thống

### 1. Chuẩn bị ESP32-CAM

- Mở file `ESP32 CAM/esp32cam.ino`.
- Cấu hình WiFi SSID và password.
- Nạp code cho ESP32-CAM.
- Lấy địa chỉ stream HTTP/MJPEG của ESP32-CAM.

### 2. Chuẩn bị AI Server

- Cài đặt Python và các thư viện cần thiết.

Ví dụ:

```bash
pip install opencv-python numpy paho-mqtt
```

- Đảm bảo trong thư mục `AI detect/` có các file:

```text
ai_human_mqtt.py
coco.names
yolov3.cfg
yolov3.weights
haarcascade_frontalface_default.xml
```

- Cấu hình trong file Python:
  - URL stream của ESP32-CAM
  - MQTT Broker của HiveMQ
  - Username/password MQTT nếu có
  - Topic publish trạng thái phát hiện người

- Chạy AI Server:

```bash
python "AI detect/ai_human_mqtt.py"
```

### 3. Chuẩn bị NodeMCU ESP8266

- Mở file `Code/smart_home.ino`.
- Cấu hình:
  - WiFi SSID/password
  - Blynk Auth Token
  - MQTT Broker, port, username/password
  - MQTT topic nhận trạng thái người
- Nạp code cho NodeMCU ESP8266.

### 4. Chuẩn bị Blynk

Trên Blynk, tạo dashboard để hiển thị và điều khiển:

- Trạng thái phát hiện người
- Trạng thái đèn
- Trạng thái quạt
- Nhiệt độ
- Độ ẩm
- Giá trị MQ2
- Cảnh báo khí gas/khói
- Cảnh báo thiết bị bị bỏ bật khi không có người

## Kết quả thực nghiệm

Trong quá trình thử nghiệm, hệ thống có thể phát hiện người trong điều kiện ánh sáng thông thường và truyền trạng thái nhận diện qua MQTT đến ESP8266 để điều khiển thiết bị. Tốc độ xử lý AI đạt khoảng **0,19–0,35 giây/khung hình**, tương đương khoảng **4–6 AI FPS**. Luồng hiển thị duy trì khoảng **14–18 Display FPS** nhờ xử lý song song.

Hệ thống cũng cập nhật dữ liệu môi trường lên Blynk, bao gồm nhiệt độ, độ ẩm và trạng thái khí gas/khói. Khi phát hiện khí gas vượt ngưỡng hoặc khi thiết bị vẫn bật trong lúc không có người, hệ thống có thể gửi cảnh báo đến điện thoại người dùng.

## Hạn chế

- Kết quả nhận diện phụ thuộc vào ánh sáng, góc đặt camera và chất lượng stream.
- YOLOv3 chạy trên PC/AI Server nên chưa tối ưu về điện năng nếu triển khai 24/7 cho một phòng nhỏ.
- Có thể xảy ra nhận nhầm khi camera nhìn thấy ảnh/video người trên màn hình hoặc ảnh in.
- File model `yolov3.weights` lớn, cần tải riêng thay vì lưu trực tiếp trong GitHub.

## Hướng phát triển

- Thay YOLOv3 bằng mô hình nhẹ hơn như YOLOv8 Nano.
- Triển khai xử lý tại biên bằng Raspberry Pi, Jetson Nano hoặc thiết bị Edge AI.
- Kết hợp cảm biến PIR làm cơ chế đánh thức camera/AI để tiết kiệm điện.
- Mở rộng hệ thống cho nhiều phòng.
- Lưu log hoạt động và tối ưu thuật toán điều khiển theo thói quen người dùng.
- Tích hợp thêm các thiết bị công suất lớn như điều hòa để tăng hiệu quả tiết kiệm năng lượng.

## Mục tiêu dự án

Dự án hướng đến việc xây dựng một prototype nhà thông minh có khả năng tự động nhận biết sự hiện diện của người, giám sát môi trường và điều khiển thiết bị điện. Qua đó, hệ thống giúp nâng cao tính tiện nghi, hỗ trợ tiết kiệm năng lượng và tạo nền tảng để phát triển các ứng dụng Smart Home sử dụng AI tại biên trong tương lai.
