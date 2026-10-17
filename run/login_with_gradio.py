import gradio as gr
import subprocess
import threading

process = None
all_output = ""

def start_process():
    global process, all_output
    all_output = ""

    process = subprocess.Popen(
        ["python", "youtube_uploader.py","--auth"],  # script cần nhập token
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    threading.Thread(target=read_output, daemon=True).start()
    return "Đã chạy script. Chờ yêu cầu nhập token..."

def read_output():
    global process, all_output
    for line in iter(process.stdout.readline, ''):
        all_output += line
    process.stdout.close()

def get_output():
    return all_output

def send_input(user_input):
    global process
    if process and process.stdin:
        process.stdin.write(user_input + "\n")
        process.stdin.flush()
        return "Đã gửi token"
    return "Không có tiến trình"

with gr.Blocks() as demo:
    gr.Markdown("Nhập token cho script")

    run_btn = gr.Button("Chạy script")
    output_box = gr.Textbox(lines=15)

    token_input = gr.Textbox(label="Nhập token")
    send_btn = gr.Button("Gửi token")

    refresh_btn = gr.Button("Cập nhật output")

    run_btn.click(start_process, outputs=output_box)
    send_btn.click(send_input, inputs=token_input, outputs=output_box)
    refresh_btn.click(get_output, outputs=output_box)

demo.launch(share=True)
