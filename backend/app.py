import os
import time
import secrets
import cv2
import smtplib
import base64
import numpy as np
import json
import string
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.mime.base import MIMEBase
from email.mime.image import MIMEImage
from email import encoders
from flask import Flask, render_template, request, jsonify
import requests
from dotenv import load_dotenv
from deepface import DeepFace

# Load env variables
load_dotenv()

app = Flask(__name__, template_folder="templates")

# Config from env
SMTP_HOST = os.getenv("SMTP_HOST", "smtp.gmail.com")
SMTP_PORT = int(os.getenv("SMTP_PORT", "465"))
EMAIL_USER = os.getenv("EMAIL_USER")
EMAIL_PASS = os.getenv("EMAIL_PASS")
REGISTERED_EMAIL = os.getenv("REGISTERED_EMAIL", EMAIL_USER)

AUTH_PASSWORD = os.getenv("AUTH_PASSWORD", "mani")
AUTH_PIN = os.getenv("AUTH_PIN", "1234")
AUTH_VOICE_PHRASE = os.getenv("AUTH_VOICE_PHRASE", "shankar")
AUTH_CENTER_PATTERN = os.getenv("AUTH_CENTER_PATTERN", "7-4-1-5-3-6-9")  # Center pattern string

DEVICE_IP = os.getenv("DEVICE_IP", "10.203.163.205")
DEVICE_API_KEY = os.getenv("DEVICE_API_KEY", "manishankar")

OTP_TTL = int(os.getenv("OTP_TTL", "180"))
SESSION_TTL = int(os.getenv("SESSION_TTL", "3600"))
QR_SESSION_TTL = int(os.getenv("QR_SESSION_TTL", "180"))

REGISTERED_FACE_DIR = os.getenv("REGISTERED_FACE_DIR", "registered_faces")
EMBEDDINGS_PATH = os.getenv("EMBEDDINGS_PATH", "embeddings.json")
FACE_MODEL = os.getenv("FACE_MODEL", "Facenet")
DETECTOR_BACKEND = os.getenv("DETECTOR_BACKEND", "mtcnn")
FACE_MATCH_THRESHOLD = float(os.getenv("FACE_MATCH_THRESHOLD", "0.40"))

NODEMCU_IP = os.getenv("NODEMCU_IP", "10.203.163.205")
NODEMCU_QR_ENDPOINT = f"http://{NODEMCU_IP}/display_qr"

# In-memory runtime
current_otp = None
otp_expire_at = 0
sessions = {}  # token -> expiry timestamp
qr_sessions = {}  # short_token -> expiry timestamp
qr_approval_requests = {}
# Load and save face embeddings
def load_embeddings():
    if not os.path.exists(EMBEDDINGS_PATH):
        return {}
    try:
        with open(EMBEDDINGS_PATH, "r") as f:
            data = json.load(f)
        return {k: np.array(v, dtype=np.float32) for k, v in data.items()}
    except Exception as e:
        print("[ERROR] Failed to load embeddings:", e)
        return {}

def save_embeddings(embeddings):
    serial = {k: v.tolist() for k, v in embeddings.items()}
    with open(EMBEDDINGS_PATH, "w") as f:
        json.dump(serial, f, indent=2)

embeddings_db = load_embeddings()

def build_embeddings_from_images():
    print("[INFO] embeddings.json not found or empty. Building from images dir...")
    if not os.path.exists(REGISTERED_FACE_DIR):
        print("[WARN] Registered faces directory does not exist:", REGISTERED_FACE_DIR)
        return
    users = [d for d in os.listdir(REGISTERED_FACE_DIR)
             if os.path.isdir(os.path.join(REGISTERED_FACE_DIR, d))]
    for user_id in users:
        user_folder = os.path.join(REGISTERED_FACE_DIR, user_id)
        embeddings = []
        for filename in os.listdir(user_folder):
            filepath = os.path.join(user_folder, filename)
            if not os.path.isfile(filepath):
                continue
            try:
                embed = DeepFace.represent(
                    img_path=filepath,
                    model_name=FACE_MODEL,
                    detector_backend=DETECTOR_BACKEND,
                    enforce_detection=True)
                if isinstance(embed, list) and len(embed) > 0 and isinstance(embed[0], (list, np.ndarray, float, int)):
                    vec = np.array(embed, dtype=np.float32)
                elif isinstance(embed, dict) and 'embedding' in embed:
                    vec = np.array(embed['embedding'], dtype=np.float32)
                else:
                    vec = np.array(embed, dtype=np.float32)
                embeddings.append(vec)
            except Exception as e:
                print(f"[WARN] could not compute embedding for {filepath}: {e}")
        if len(embeddings) == 0:
            print(f"[WARN] no valid embeddings for user {user_id}; skipping")
            continue
        mean_embedding = np.mean(np.stack(embeddings, axis=0), axis=0)
        embeddings_db[user_id] = mean_embedding
    save_embeddings(embeddings_db)

if not embeddings_db:
    build_embeddings_from_images()

print(f"[INFO] App starting. Registered face dir: {REGISTERED_FACE_DIR}")

# Email helpers
def send_email(subject: str, html_body: str, to_addr=REGISTERED_EMAIL, attachment_path=None, inline_image=None):
    if not EMAIL_USER or not EMAIL_PASS:
        print("[WARN] EMAIL_USER/EMAIL_PASS not set — skipping email send.")
        return

    # Use "related" for HTML + inline images
    msg = MIMEMultipart("related")
    msg["Subject"] = subject
    msg["From"] = EMAIL_USER
    msg["To"] = to_addr

    # Create alternative part (HTML)
    alt_part = MIMEMultipart("alternative")
    msg.attach(alt_part)

    part = MIMEText(html_body, "html")
    alt_part.attach(part)

    # Inline image support
    if inline_image and os.path.exists(inline_image):
        with open(inline_image, "rb") as f:
            img = MIMEImage(f.read())
            img.add_header("Content-ID", "<captureimg>")  # same cid used in HTML
            img.add_header("Content-Disposition", "inline", filename=os.path.basename(inline_image))
            msg.attach(img)

    # Normal attachment (still supported)
    if attachment_path and os.path.exists(attachment_path):
        with open(attachment_path, "rb") as f:
            mime_base = MIMEBase('application', 'octet-stream')
            mime_base.set_payload(f.read())
            encoders.encode_base64(mime_base)
            mime_base.add_header('Content-Disposition', f'attachment; filename={os.path.basename(attachment_path)}')
            msg.attach(mime_base)

    # Send email
    try:
        with smtplib.SMTP_SSL(SMTP_HOST, SMTP_PORT) as server:
            server.login(EMAIL_USER, EMAIL_PASS)
            server.sendmail(EMAIL_USER, to_addr, msg.as_string())
        print("[INFO] Email sent:", subject)
    except Exception as e:
        print("[ERROR] Failed to send email:", e)

def send_otp_email(otp: str):
    subject = "Your OTP for Smart Lock"
    html = f"<p>Your OTP is: <b>{otp}</b></p><p>Expires in {OTP_TTL//60} minute(s).</p>"
    send_email(subject, html)

def send_alert_email(reason, remote_ip, attachment_path=None):
    subject = f"Alert: {reason}"
    html = f"<p>Alert reason: <b>{reason}</b></p><p>From IP: {remote_ip}</p>"
    send_email(subject, html, attachment_path=attachment_path)

def send_success_email(method, remote_ip):
    subject = f"Success: {method} authentication"
    html = f"<p>{method} authentication succeeded.</p><p>From IP: {remote_ip}</p>"
    send_email(subject, html)

# Utility helpers
def gen_otp():
    return ''.join([str(secrets.randbelow(10)) for _ in range(6)])

def gen_token():
    return secrets.token_urlsafe(24)

def gen_short_token(length=6):
    alphabet = string.ascii_letters + string.digits
    return ''.join(secrets.choice(alphabet) for _ in range(length))

def token_valid(token: str):
    exp = sessions.get(token)
    return exp is not None and exp > time.time()

def qr_session_valid(token: str):
    exp = qr_sessions.get(token)
    return exp is not None and exp > time.time()

def cosine_distance(a: np.ndarray, b: np.ndarray):
    a = a.astype(np.float32)
    b = b.astype(np.float32)
    denom = (np.linalg.norm(a) * np.linalg.norm(b)) + 1e-10
    return 1.0 - float(np.dot(a, b) / denom)

def capture_image():
    try:
        cam = cv2.VideoCapture(0, cv2.CAP_DSHOW)
        if not cam.isOpened():
            cam.release()
            print("[WARN] Camera not accessible.")
            return None
        ret, frame = cam.read()
        cam.release()
        if ret:
            os.makedirs("intruder_images", exist_ok=True)
            filename = os.path.join("intruder_images", f"intruder_{int(time.time())}.jpg")
            cv2.imwrite(filename, frame)
            print(f"[INFO] Intruder image saved: {filename}")
            return filename
    except Exception as e:
        print("[WARN] capture_image exception:", e)
    print("[WARN] Failed to capture image.")
    return None

def nodemcu_control(action: str):
    url = f"http://{DEVICE_IP}/control"
    payload = {"action": action, "key": DEVICE_API_KEY}
    try:
        res = requests.post(url, json=payload, timeout=5)
        res.raise_for_status()
        if res.headers.get("Content-Type", "").startswith("application/json"):
            return res.json()
        return {"ok": False, "error": "Invalid response from device"}
    except requests.exceptions.RequestException as e:
        return {"ok": False, "error": f"NodeMCU communication error: {str(e)}"}

def nodemcu_qr_display(url: str, name: str, phone: str):
    payload = {"url": url, "name": name, "phone": phone, "key": DEVICE_API_KEY}
    try:
        res = requests.post(NODEMCU_QR_ENDPOINT, json=payload, timeout=5)
        res.raise_for_status()
        return res.json()
    except requests.exceptions.RequestException as e:
        return {"ok": False, "error": f"NodeMCU QR communication error: {str(e)}"}

# ------------------ Face registration/login ------------------
@app.route("/register_face", methods=["POST"])
def register_face():
    data = request.get_json() or {}
    images_b64 = data.get("images", [])
    user_id = data.get("user_id", "").strip()
    if not user_id:
        return jsonify({"ok": False, "error": "user_id required"}), 400
    if not images_b64 or not isinstance(images_b64, list):
        return jsonify({"ok": False, "error": "No images provided or invalid format"}), 400

    user_folder = os.path.join(REGISTERED_FACE_DIR, user_id)
    os.makedirs(user_folder, exist_ok=True)

    collected_embeddings = []
    saved_count = 0

    for idx, img_b64 in enumerate(images_b64):
        try:
            header, b64 = img_b64.split(",", 1) if "," in img_b64 else ("", img_b64)
            img_bytes = base64.b64decode(b64)
            np_img = np.frombuffer(img_bytes, np.uint8)
            bgr = cv2.imdecode(np_img, cv2.IMREAD_COLOR)
            if bgr is None:
                continue
            filename = os.path.join(user_folder, f"{user_id}_{int(time.time())}_{idx}.jpg")
            cv2.imwrite(filename, bgr)
            saved_count += 1
            try:
                embed = DeepFace.represent(
                    img_path=filename,
                    model_name=FACE_MODEL,
                    detector_backend=DETECTOR_BACKEND,
                    enforce_detection=True)
                if isinstance(embed, list) and len(embed) > 0 and isinstance(embed[0], (list, np.ndarray, float, int)):
                    vec = np.array(embed[0], dtype=np.float32)
                elif isinstance(embed, dict) and 'embedding' in embed:
                    vec = np.array(embed['embedding'], dtype=np.float32)
                else:
                    vec = np.array(embed, dtype=np.float32)
                collected_embeddings.append(vec)
            except Exception as e:
                print(f"[WARN] embedding failed for saved image {filename}: {e}")
                continue
        except Exception as e:
            print("[ERROR] Exception during face save:", e)
            continue

    if saved_count == 0:
        return jsonify({"ok": False, "error": "No valid images uploaded."}), 400
    if len(collected_embeddings) == 0:
        return jsonify({"ok": False, "error": "No faces detected in provided images."}), 400

    mean_embedding = np.mean(np.stack(collected_embeddings, axis=0), axis=0)
    embeddings_db[user_id] = mean_embedding
    save_embeddings(embeddings_db)
    return jsonify({"ok": True, "message": f"Registered {saved_count} images; embeddings saved for user '{user_id}'."})

@app.route("/face-login", methods=["POST"])
def face_login():
    data = request.get_json() or {}
    img_data = data.get("image", "")
    remote_ip = request.remote_addr
    if not img_data:
        return jsonify({"ok": False, "error": "No image provided"}), 400
    try:
        header, b64 = img_data.split(",", 1) if "," in img_data else ("", img_data)
        img_bytes = base64.b64decode(b64)
        np_img = np.frombuffer(img_bytes, np.uint8)
        bgr = cv2.imdecode(np_img, cv2.IMREAD_COLOR)
        if bgr is None:
            return jsonify({"ok": False, "error": "Invalid image data"}), 400
    except Exception:
        return jsonify({"ok": False, "error": "Invalid image data"}), 400

    if not embeddings_db:
        return jsonify({"ok": False, "error": "No registered users. Please register first."}), 400

    try:
        temp_img_path = f"temp_face_{int(time.time())}.jpg"
        cv2.imwrite(temp_img_path, bgr)
        embed = DeepFace.represent(
            img_path=temp_img_path,
            model_name=FACE_MODEL,
            detector_backend=DETECTOR_BACKEND,
            enforce_detection=True)
        os.remove(temp_img_path)
        if isinstance(embed, dict) and "embedding" in embed:
            probe = np.array(embed["embedding"], dtype=np.float32)
        elif isinstance(embed, list) and len(embed) > 0:
            first = embed[0]
            if isinstance(first, dict) and "embedding" in first:
                probe = np.array(first["embedding"], dtype=np.float32)
            else:
                probe = np.array(first, dtype=np.float32)
        else:
            probe = np.array(embed, dtype=np.float32)
    except Exception as e:
        print("[INFO] Face detection/embedding failed:", e)
        img_path = capture_image()
        send_alert_email("Face detection failed or no face in the image", remote_ip, img_path)
        return jsonify({"ok": False, "error": "Face not detected / could not compute embedding"}), 400

    best_user = None
    best_distance = float("inf")
    for user_id, stored_vec in embeddings_db.items():
        d = cosine_distance(probe, stored_vec)
        print(f"[DEBUG] compare to {user_id}: distance={d:.4f}")
        if d < best_distance:
            best_distance = d
            best_user = user_id

    print(f"[INFO] Best match: {best_user} distance={best_distance:.4f} (threshold={FACE_MATCH_THRESHOLD})")
    if best_distance <= FACE_MATCH_THRESHOLD:
        send_success_email("face", remote_ip)
        token = gen_token()
        sessions[token] = time.time() + SESSION_TTL
        return jsonify({"ok": True, "token": token, "expires_in": SESSION_TTL, "message": f"Face recognized: {best_user}", "distance": float(best_distance)})
    else:
        img_path = None
        try:
            os.makedirs("intruder_images", exist_ok=True)
            img_path = os.path.join("intruder_images", f"face_fail_{int(time.time())}.jpg")
            cv2.imwrite(img_path, bgr)
        except Exception as e:
            print("[WARN] Failed to save intruder image:", e)
        send_alert_email("Face not recognized", remote_ip, img_path)
        return jsonify({"ok": False, "error": "Face not recognized", "best_distance": float(best_distance)}), 401

# ------------------ Basic pages ------------------
@app.route("/")
def index():
    return render_template("index.html")

@app.route("/mc/<token>")
def mobile_control(token):
    if qr_session_valid(token):
        return render_template("mobile_control.html", token=token)
    return "Invalid or expired QR session.", 401

# ------------------ OTP / Password / PIN / Voice auth ------------------
@app.route("/request_otp", methods=["POST"])
def request_otp():
    global current_otp, otp_expire_at
    current_otp = gen_otp()
    otp_expire_at = time.time() + OTP_TTL
    send_otp_email(current_otp)
    return jsonify({"ok": True, "message": "OTP sent to registered email."})

@app.route("/auth", methods=["POST"])
def auth():
    data = request.get_json() or {}
    method = data.get("method")
    value = data.get("value", "").strip()
    remote_ip = request.remote_addr

    ok = False
    reason = ""
    if method == "password":
        if value == AUTH_PASSWORD:
            ok = True
        else:
            reason = "Wrong password"
    elif method == "pin":
        if value == AUTH_PIN:
            ok = True
        else:
            reason = "Wrong PIN"
    elif method == "otp":
        now = time.time()
        if current_otp and now < otp_expire_at and value == current_otp:
            ok = True
        else:
            reason = "Wrong or expired OTP"
    elif method == "voice":
        if value.lower() == AUTH_VOICE_PHRASE.lower():
            ok = True
        else:
            reason = "Wrong voice phrase"
    elif method == "centerpattern":
        if value == AUTH_CENTER_PATTERN:
            ok = True
        else:
            reason = "Wrong center pattern"
    else:
        return jsonify({"ok": False, "error": "Invalid method"}), 400

    if not ok:
        img_path = capture_image()
        send_alert_email(reason + f" (method={method})", remote_ip, img_path)
        return jsonify({"ok": False, "error": reason}), 401

    send_success_email(method, remote_ip)
    token = gen_token()
    sessions[token] = time.time() + SESSION_TTL
    return jsonify({"ok": True, "token": token, "expires_in": SESSION_TTL})


@app.route("/request_qr", methods=["POST"])
def request_qr():
    data = request.get_json() or {}
    name = data.get("name")
    phone = data.get("phone")
    if not name or not phone:
        return jsonify({"ok": False, "error": "Name and phone number are required."}), 400
    
    approval_token = gen_token()
    qr_approval_requests[approval_token] = {
        "name": name,
        "phone": phone,
        "status": "pending",
        "timestamp": time.time()
    }

    base_url = "http://10.203.163.227:5000"  # Replace with your actual IP/domain

    approve_link = f"{base_url}/qr/approve?token={approval_token}"
    deny_link = f"{base_url}/qr/deny?token={approval_token}"

    # 📸 Capture image
    img_path = capture_image()

    email_html = f"""
    <h3>QR Code Request</h3>
    <p>Name: <b>{name}</b></p>
    <p>Phone: <b>{phone}</b></p>
    <p>Please <a href="{approve_link}">Approve</a> or <a href="{deny_link}">Deny</a> this request.</p>
    <br>
    <p><b>Attached is the captured image for verification :</b></p>
    <img src="cid:captureimg" width="300">
    """

    send_email("Smart Lock QR Code Request Approval", email_html, inline_image=img_path)
    
    return jsonify({
        'ok': True,
        'message': 'Request sent for approval. Check your email.',
        'token': approval_token
    })

@app.route("/qr/approve")
def qr_approve():
    token = request.args.get("token")
    req = qr_approval_requests.get(token)
    if not req or req["status"] != "pending":
        return "Invalid or expired approval link.", 400

    req["status"] = "approved"
    qr_token = gen_short_token()
    qr_sessions[qr_token] = time.time() + QR_SESSION_TTL

    base_url = "http://10.203.163.227:5000"
    qr_url = f"{base_url}/mc/{qr_token}"

    device_resp = nodemcu_qr_display(qr_url, req["name"], req["phone"])
    # Log or handle device_resp if needed

    # Keep approval request for status polling, cleanup separately later

    return f"""
    <html style='font-family:sans-serif; text-align:center; padding:40px;'>
    <h1>✅ QR Code Request Approved</h1>
    <p>The QR code has been generated and sent to the device.</p>
    <p>You can now scan it with your mobile.</p>
    <p><a href='/'>Return to dashboard</a></p>
    </html>
    """

@app.route("/qr/deny")
def qr_deny():
    token = request.args.get("token")
    req = qr_approval_requests.get(token)
    if not req or req["status"] != "pending":
        return "Invalid or expired denial link.", 400

    req["status"] = "denied"
    # Optionally keep request for some time for polling

    return """
    <html style='font-family:sans-serif; text-align:center; padding:40px;'>
    <h1>❌ QR Code Request Denied</h1>
    <p>Your QR code request was denied. No QR code generated.</p>
    <p><a href='/'>Return to dashboard</a></p>
    </html>
    """

@app.route('/qr_status')
def qr_status():
    token = request.args.get('token')
    if not token or token not in qr_approval_requests:
        return jsonify({'ok': False, 'error': 'Invalid or missing token'})
    status = qr_approval_requests[token]['status']
    return jsonify({'ok': True, 'status': status})
@app.route("/control", methods=["POST"])
def control():
    auth_header = request.headers.get("Authorization", "")
    token = ""
    if auth_header.startswith("Bearer "):
        token = auth_header.split(" ", 1)[1].strip()
    else:
        data = request.get_json(silent=True) or {}
        token = data.get("token", "")

    if not token or not token_valid(token):
        if not token or not qr_session_valid(token):
            return jsonify({"ok": False, "error": "Unauthorized or expired session"}), 401

    data = request.get_json() or {}
    action = data.get("action")
    if action not in ("unlock", "close"):
        return jsonify({"ok": False, "error": "Invalid action"}), 400

    try:
        device_resp = nodemcu_control(action)
        return jsonify({"ok": True, "device": device_resp})
    except Exception as e:
        return jsonify({"ok": False, "error": "Device error: " + str(e)}), 500

@app.route("/status", methods=["GET"])
def status():
    try:
        url = f"http://{DEVICE_IP}/status?key={DEVICE_API_KEY}"
        res = requests.get(url, timeout=5)
        return jsonify({"ok": True, "device": res.json()})
    except Exception as e:
        return jsonify({"ok": False, "error": "Device error: " + str(e)}), 500

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=True)











