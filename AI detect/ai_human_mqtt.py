import cv2
import numpy as np
import time
import os
import ssl
import threading
import paho.mqtt.client as mqtt
from collections import OrderedDict
import sys
sys.stdout.reconfigure(encoding='utf-8')

# =========================================================
# NGUỒN CAMERA
# =========================================================
SOURCE_MODE = "esp32_stream"

# ESP32_CAM_STREAM_URL = ""

FRAME_WIDTH = 960
FRAME_HEIGHT = 540

# AI chạy nền mỗi 0.5 giây
AI_INTERVAL = 0.5

# Nếu status không đổi, vẫn gửi lại MQTT mỗi 3 giây
MQTT_HEARTBEAT_INTERVAL = 3.0

# =========================================================
# FILE YOLO
# =========================================================
YOLO_WEIGHTS = "yolov3.weights"
YOLO_CONFIG = "yolov3.cfg"
COCO_NAMES = "coco.names"

# =========================================================
# YOLO CONFIG
# =========================================================
INPUT_SIZE = 416
CONF_THRESHOLD = 0.25
NMS_THRESHOLD = 0.4

PERSON_CLASS_ID = 0

# =========================================================
# PERSON BOX FILTER
# =========================================================
MIN_PERSON_BOX_AREA = 500
MIN_PERSON_BOX_HEIGHT = 25
MIN_HEIGHT_WIDTH_RATIO = 0.2
MAX_HEIGHT_WIDTH_RATIO = 10.0

# =========================================================
# FACE DETECTION
# =========================================================
USE_FACE_DETECTION = True
DRAW_FACE_BOX = False

FACE_MIN_SIZE = 90
FACE_CONFIRM_FRAMES = 4

FACE_SCALE_FACTOR = 1.08
FACE_MIN_NEIGHBORS = 8

MIN_FACE_AREA = 7000
MIN_FACE_RATIO = 0.65
MAX_FACE_RATIO = 1.45

# =========================================================
# HUMAN STATUS
# =========================================================

NO_PERSON_TIMEOUT = 20.0

# =========================================================
# TRACKER
# =========================================================
MAX_DISAPPEARED = 10
MAX_DISTANCE = 250

# =========================================================
# MQTT - HIVEMQ CLOUD
# =========================================================
MQTT_ENABLE = True

# MQTT_BROKER = ""
# MQTT_PORT =

# MQTT_USERNAME = ""
# MQTT_PASSWORD = ""

MQTT_TOPIC = "esp32-cam"

# =========================================================
# SHARED STATE
# =========================================================
latest_frame = None

latest_state = {
    "person_boxes": [],
    "face_boxes": [],
    "objects": OrderedDict(),

    "status": "HUMAN_NO_DETECTED",
    "human_detected": False,
    "person_count": 0,
    "face_count": 0,
    "detect_reason": "not_started",

    "face_confirm_count": 0,
    "required_face_frames": FACE_CONFIRM_FRAMES,
    "no_person_elapsed": 999.0,
    "process_time": 0.0,
    "ai_fps": 0.0,
    "last_ai_time": 0.0
}

frame_lock = threading.Lock()
state_lock = threading.Lock()
stop_event = threading.Event()


# =========================================================
# TRACKER THEO TÂM BOX PERSON
# =========================================================
class CentroidTracker:
    def __init__(self, maxDisappeared=10, maxDistance=250):
        self.nextObjectID = 0
        self.objects = OrderedDict()
        self.disappeared = OrderedDict()
        self.maxDisappeared = maxDisappeared
        self.maxDistance = maxDistance

    def register(self, centroid):
        self.objects[self.nextObjectID] = centroid
        self.disappeared[self.nextObjectID] = 0
        self.nextObjectID += 1

    def deregister(self, objectID):
        if objectID in self.objects:
            del self.objects[objectID]
        if objectID in self.disappeared:
            del self.disappeared[objectID]

    def update(self, rects):
        if len(rects) == 0:
            for objectID in list(self.disappeared.keys()):
                self.disappeared[objectID] += 1

                if self.disappeared[objectID] > self.maxDisappeared:
                    self.deregister(objectID)

            return self.objects.copy()

        inputCentroids = np.zeros((len(rects), 2), dtype="int")

        for i, (startX, startY, endX, endY) in enumerate(rects):
            cX = int((startX + endX) / 2.0)
            cY = int((startY + endY) / 2.0)
            inputCentroids[i] = (cX, cY)

        if len(self.objects) == 0:
            for centroid in inputCentroids:
                self.register(centroid)

            return self.objects.copy()

        objectIDs = list(self.objects.keys())
        objectCentroids = list(self.objects.values())

        D = np.linalg.norm(
            np.array(objectCentroids)[:, np.newaxis] - inputCentroids,
            axis=2
        )

        rows = D.min(axis=1).argsort()
        cols = D.argmin(axis=1)[rows]

        usedRows = set()
        usedCols = set()

        for row, col in zip(rows, cols):
            if row in usedRows or col in usedCols:
                continue

            if D[row, col] > self.maxDistance:
                continue

            objectID = objectIDs[row]
            self.objects[objectID] = inputCentroids[col]
            self.disappeared[objectID] = 0

            usedRows.add(row)
            usedCols.add(col)

        unusedRows = set(range(D.shape[0])).difference(usedRows)
        unusedCols = set(range(D.shape[1])).difference(usedCols)

        for row in unusedRows:
            objectID = objectIDs[row]
            self.disappeared[objectID] += 1

            if self.disappeared[objectID] > self.maxDisappeared:
                self.deregister(objectID)

        for col in unusedCols:
            self.register(inputCentroids[col])

        return self.objects.copy()


# =========================================================
# KIỂM TRA FILE
# =========================================================
def check_required_files():
    required_files = [YOLO_WEIGHTS, YOLO_CONFIG, COCO_NAMES]

    for file_path in required_files:
        if not os.path.exists(file_path):
            print(f"[LỖI] Không tìm thấy file: {file_path}")
            print("Hãy đặt file này cùng thư mục với ai_human_mqtt.py")
            return False

    return True


# =========================================================
# LOAD MODEL
# =========================================================
def load_models():
    print("[INFO] Đang load YOLOv3...")

    net = cv2.dnn.readNet(YOLO_WEIGHTS, YOLO_CONFIG)
    net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
    net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)

    with open(COCO_NAMES, "r", encoding="utf-8") as f:
        classes = [line.strip() for line in f.readlines()]

    layer_names = net.getLayerNames()
    output_layers = [
        layer_names[i - 1]
        for i in net.getUnconnectedOutLayers().flatten()
    ]

    print("[INFO] Load YOLOv3 xong.")

    print("[INFO] Đang load Face Detector...")

    face_cascade_path = cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
    face_cascade = cv2.CascadeClassifier(face_cascade_path)

    if face_cascade.empty():
        print("[LỖI] Không load được Haar Cascade face detector.")
        return None, None, None, None

    print("[INFO] Load Face Detector xong.")

    return net, classes, output_layers, face_cascade


# =========================================================
# MQTT
# =========================================================
def create_mqtt_client():
    try:
        return mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="ai-human-detection-client"
        )
    except Exception:
        return mqtt.Client(client_id="ai-human-detection-client")


def setup_mqtt():
    if not MQTT_ENABLE:
        print("[MQTT] MQTT đang tắt.")
        return None

    try:
        client = create_mqtt_client()
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

        client.tls_set(
            cert_reqs=ssl.CERT_REQUIRED,
            tls_version=ssl.PROTOCOL_TLS_CLIENT
        )

        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        client.loop_start()

        print("[MQTT] Đã kết nối HiveMQ Cloud.")
        print(f"[MQTT] Broker: {MQTT_BROKER}")
        print(f"[MQTT] Topic publish: {MQTT_TOPIC}")

        return client

    except Exception as e:
        print("[MQTT LỖI] Không kết nối được MQTT:", e)
        return None


def publish_mqtt(client, payload):
    if not MQTT_ENABLE or client is None:
        return False

    try:
        message = str(payload)
        result = client.publish(MQTT_TOPIC, message, qos=0, retain=False)

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            print("[MQTT] Đã gửi:", message)
            return True

        print("[MQTT LỖI] Publish thất bại, rc =", result.rc)
        return False

    except Exception as e:
        print("[MQTT LỖI] Publish exception:", e)
        return False


# =========================================================
# MỞ ESP32-CAM STREAM
# =========================================================
def open_esp32_stream():
    print("[INFO] Đang mở ESP32-CAM stream...")
    print("[INFO] URL:", ESP32_CAM_STREAM_URL)

    cap = cv2.VideoCapture(ESP32_CAM_STREAM_URL)

    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    if not cap.isOpened():
        print("[LỖI] Không mở được ESP32-CAM stream.")
        print("Hãy kiểm tra lại link trên trình duyệt:")
        print(ESP32_CAM_STREAM_URL)
        return None

    print("[INFO] Đã mở ESP32-CAM stream thành công.")
    return cap


# =========================================================
# DETECTION HELPERS
# =========================================================
def is_valid_person_box(x, y, boxW, boxH):
    area = boxW * boxH

    if area < MIN_PERSON_BOX_AREA:
        return False

    if boxH < MIN_PERSON_BOX_HEIGHT:
        return False

    height_width_ratio = boxH / max(boxW, 1)

    if height_width_ratio < MIN_HEIGHT_WIDTH_RATIO:
        return False

    if height_width_ratio > MAX_HEIGHT_WIDTH_RATIO:
        return False

    return True


def is_valid_face_box(x, y, w, h):
    area = w * h

    if area < MIN_FACE_AREA:
        return False

    ratio = w / max(h, 1)

    if ratio < MIN_FACE_RATIO:
        return False

    if ratio > MAX_FACE_RATIO:
        return False

    return True


def apply_nms(boxes, confidences):
    final_boxes = []

    if len(boxes) == 0:
        return final_boxes

    indices = cv2.dnn.NMSBoxes(
        boxes,
        confidences,
        CONF_THRESHOLD,
        NMS_THRESHOLD
    )

    if len(indices) > 0:
        for i in indices.flatten():
            x, y, w, h = boxes[i]
            conf = confidences[i]

            final_boxes.append({
                "x": int(x),
                "y": int(y),
                "w": int(w),
                "h": int(h),
                "confidence": round(float(conf), 3)
            })

    return final_boxes


def detect_yolo_person(frame, net, output_layers):
    h, w = frame.shape[:2]

    blob = cv2.dnn.blobFromImage(
        frame,
        scalefactor=1 / 255.0,
        size=(INPUT_SIZE, INPUT_SIZE),
        swapRB=True,
        crop=False
    )

    net.setInput(blob)
    outputs = net.forward(output_layers)

    person_boxes = []
    person_confidences = []

    for output in outputs:
        for det in output:
            objectness = float(det[4])
            scores = det[5:]

            class_id = int(np.argmax(scores))
            class_score = float(scores[class_id])
            confidence = objectness * class_score

            if class_id != PERSON_CLASS_ID:
                continue

            if confidence < CONF_THRESHOLD:
                continue

            centerX = int(det[0] * w)
            centerY = int(det[1] * h)
            boxW = int(det[2] * w)
            boxH = int(det[3] * h)

            x = int(centerX - boxW / 2)
            y = int(centerY - boxH / 2)

            x = max(0, x)
            y = max(0, y)
            boxW = min(boxW, w - x)
            boxH = min(boxH, h - y)

            if boxW <= 0 or boxH <= 0:
                continue

            if not is_valid_person_box(x, y, boxW, boxH):
                continue

            person_boxes.append([x, y, boxW, boxH])
            person_confidences.append(float(confidence))

    final_person_boxes = apply_nms(person_boxes, person_confidences)

    rects = []
    for box in final_person_boxes:
        x = box["x"]
        y = box["y"]
        boxW = box["w"]
        boxH = box["h"]
        rects.append((x, y, x + boxW, y + boxH))

    return rects, final_person_boxes


def detect_faces(frame, face_cascade):
    if not USE_FACE_DETECTION:
        return []

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    gray = cv2.equalizeHist(gray)

    faces = face_cascade.detectMultiScale(
        gray,
        scaleFactor=FACE_SCALE_FACTOR,
        minNeighbors=FACE_MIN_NEIGHBORS,
        minSize=(FACE_MIN_SIZE, FACE_MIN_SIZE)
    )

    final_faces = []

    for (x, y, w, h) in faces:
        if not is_valid_face_box(x, y, w, h):
            continue

        final_faces.append({
            "x": int(x),
            "y": int(y),
            "w": int(w),
            "h": int(h)
        })

    return final_faces


# =========================================================
# AI WORKER THREAD
# =========================================================
def ai_worker(net, output_layers, face_cascade, mqtt_client):
    global latest_frame, latest_state

    tracker = CentroidTracker(
        maxDisappeared=MAX_DISAPPEARED,
        maxDistance=MAX_DISTANCE
    )

    last_seen_time = 0.0
    face_confirm_count = 0

    last_ai_run = 0.0
    last_mqtt_publish = 0.0
    last_status = None

    ai_fps_smooth = 0.0

    while not stop_event.is_set():
        now = time.time()

        if now - last_ai_run < AI_INTERVAL:
            time.sleep(0.01)
            continue

        last_ai_run = now

        with frame_lock:
            if latest_frame is None:
                continue
            frame = latest_frame.copy()

        start_time = time.time()

        # 1. Detect person bằng YOLO
        rects, person_boxes = detect_yolo_person(
            frame,
            net,
            output_layers
        )

        person_count = len(person_boxes)

        # 2. Nếu không thấy person thì detect face
        face_boxes = []

        if person_count == 0 and USE_FACE_DETECTION:
            face_boxes = detect_faces(frame, face_cascade)

        face_count = len(face_boxes)

        # 3. Tracker chỉ track person
        objects = tracker.update(rects)

        # 4. Face phải ổn định nhiều lần mới xác nhận
        if face_count > 0:
            face_confirm_count += 1
        else:
            face_confirm_count = 0

        required_face_frames = FACE_CONFIRM_FRAMES
        face_confirmed = face_confirm_count >= required_face_frames

        detected_by_person = person_count > 0
        detected_by_face = person_count == 0 and face_confirmed

        # 5. Person hoặc Face đều quy về human detected
        detected_now = detected_by_person or detected_by_face

        if detected_now:
            last_seen_time = time.time()

        if last_seen_time == 0.0:
            no_person_elapsed = 999.0
        else:
            no_person_elapsed = time.time() - last_seen_time

        human_detected = no_person_elapsed < NO_PERSON_TIMEOUT

        if detected_by_person:
            detect_reason = "person_detected"
        elif detected_by_face:
            detect_reason = "face_detected"
        elif human_detected:
            detect_reason = "hold_last_seen_timeout"
        else:
            detect_reason = "no_detection"

        status = "HUMAN_DETECTED" if human_detected else "HUMAN_NO_DETECTED"

        process_time = time.time() - start_time
        instant_ai_fps = 1.0 / max(process_time, 1e-6)

        if ai_fps_smooth == 0.0:
            ai_fps_smooth = instant_ai_fps
        else:
            ai_fps_smooth = (ai_fps_smooth * 0.8) + (instant_ai_fps * 0.2)

        payload = status

        # Gửi MQTT khi status đổi hoặc tới heartbeat interval
        should_publish = False

        if status != last_status:
            should_publish = True
            last_status = status

        if time.time() - last_mqtt_publish >= MQTT_HEARTBEAT_INTERVAL:
            should_publish = True

        if should_publish:
            last_mqtt_publish = time.time()
            publish_mqtt(mqtt_client, payload)

        with state_lock:
            latest_state = {
                "person_boxes": person_boxes,
                "face_boxes": face_boxes,
                "objects": objects,

                "status": status,
                "human_detected": human_detected,
                "person_count": person_count,
                "face_count": face_count,
                "detect_reason": detect_reason,

                "face_confirm_count": face_confirm_count,
                "required_face_frames": required_face_frames,
                "no_person_elapsed": no_person_elapsed,
                "process_time": process_time,
                "ai_fps": ai_fps_smooth,
                "last_ai_time": time.time()
            }

        print(
            f"Status: {status} | "
            f"Reason: {detect_reason} | "
            f"Person: {person_count} | "
            f"Face: {face_count} | "
            f"AI time: {process_time:.3f}s"
        )


# =========================================================
# DRAW
# =========================================================
def draw_display(frame, state, display_fps):
    person_boxes = state["person_boxes"]
    face_boxes = state["face_boxes"]
    objects = state["objects"]

    status = state["status"]
    human_detected = state["human_detected"]
    person_count = state["person_count"]
    face_count = state["face_count"]
    detect_reason = state["detect_reason"]
    face_confirm_count = state["face_confirm_count"]
    required_face_frames = state["required_face_frames"]
    no_person_elapsed = state["no_person_elapsed"]
    process_time = state["process_time"]
    ai_fps = state["ai_fps"]

    # Vẽ person box
    for box in person_boxes:
        x = box["x"]
        y = box["y"]
        w = box["w"]
        h = box["h"]
        conf = box["confidence"]

        cv2.rectangle(frame, (x, y), (x + w, y + h), (255, 0, 0), 2)

        cv2.putText(
            frame,
            f"Person {conf:.2f}",
            (x, max(25, y - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            (255, 0, 0),
            2
        )

    # Face box mặc định không vẽ để giao diện gọn
    if DRAW_FACE_BOX:
        for face in face_boxes:
            x = face["x"]
            y = face["y"]
            w = face["w"]
            h = face["h"]

            cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 255), 2)

            cv2.putText(
                frame,
                "Face",
                (x, max(25, y - 8)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (0, 255, 255),
                2
            )

    # Không vẽ ID tracker nữa để tránh rối

    status_color = (0, 0, 255) if human_detected else (0, 255, 255)

    cv2.putText(
        frame,
        f"Status: {status}",
        (30, 40),
        cv2.FONT_HERSHEY_SIMPLEX,
        1,
        status_color,
        3
    )

    cv2.putText(
        frame,
        f"Reason: {detect_reason}",
        (30, 80),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (0, 255, 0),
        2
    )

    cv2.putText(
        frame,
        f"Person: {person_count} | Face: {face_count}",
        (30, 115),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (0, 255, 0),
        2
    )

    cv2.putText(
        frame,
        f"Face confirm: {face_confirm_count}/{required_face_frames}",
        (30, 150),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (0, 255, 255),
        2
    )

    show_no_person_time = 0.0 if human_detected else no_person_elapsed

    cv2.putText(
        frame,
        f"No human time: {show_no_person_time:.1f}s",
        (30, 185),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (0, 255, 255),
        2
    )

    cv2.putText(
        frame,
        f"Display FPS: {display_fps:.1f} | AI FPS: {ai_fps:.1f}",
        (30, 220),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 0),
        2
    )

    cv2.putText(
        frame,
        f"AI time: {process_time:.3f}s | MQTT: {MQTT_TOPIC}",
        (30, 255),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.7,
        (255, 255, 255),
        2
    )


# =========================================================
# MAIN
# =========================================================
def main():
    global latest_frame

    if not check_required_files():
        return

    net, classes, output_layers, face_cascade = load_models()

    if net is None or face_cascade is None:
        return

    mqtt_client = setup_mqtt()

    cap = open_esp32_stream()

    if cap is None:
        return

    worker = threading.Thread(
        target=ai_worker,
        args=(net, output_layers, face_cascade, mqtt_client),
        daemon=True
    )
    worker.start()

    cv2.namedWindow("AI Human Detection + MQTT", cv2.WINDOW_NORMAL)

    print("[INFO] Bắt đầu chạy ESP32-CAM stream + AI + MQTT.")
    print("[INFO] Nhấn Q để thoát.")

    prev_display_time = time.time()
    display_fps_smooth = 0.0

    reconnect_delay = 1.0

    while True:
        ret, frame = cap.read()

        if not ret:
            print("[LỖI] Không đọc được frame từ ESP32-CAM stream. Đang thử kết nối lại...")
            cap.release()
            time.sleep(reconnect_delay)
            cap = open_esp32_stream()

            if cap is None:
                time.sleep(reconnect_delay)
                continue

            continue

        frame = cv2.resize(frame, (FRAME_WIDTH, FRAME_HEIGHT))

        with frame_lock:
            latest_frame = frame.copy()

        with state_lock:
            state_snapshot = latest_state.copy()

        now_display = time.time()
        instant_display_fps = 1.0 / max(now_display - prev_display_time, 1e-6)
        prev_display_time = now_display

        if display_fps_smooth == 0.0:
            display_fps_smooth = instant_display_fps
        else:
            display_fps_smooth = (display_fps_smooth * 0.9) + (instant_display_fps * 0.1)

        draw_frame = frame.copy()
        draw_display(draw_frame, state_snapshot, display_fps_smooth)

        cv2.imshow("AI Human Detection + MQTT", draw_frame)

        key = cv2.waitKey(1) & 0xFF

        if key == ord("q"):
            print("[INFO] Đang thoát chương trình...")
            break

    stop_event.set()
    worker.join(timeout=2)

    cap.release()

    if mqtt_client is not None:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()

    cv2.destroyAllWindows()
    print("[INFO] Đã thoát.")


if __name__ == "__main__":
    main()