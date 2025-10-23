import subprocess
import tkinter as tk
from tkinter import ttk
from threading import Thread
import os
from PIL import Image, ImageTk

# -------------------------------
# Configuración
# -------------------------------
EXECUTABLE = "./build/Debug/dotprod_mesi.exe"   # Ruta a tu ejecutable C++
METRICS_SCRIPT = "./metrics.py"                 # Script Python de métricas
CACHE_CSV = "./cache_stats.csv"                 # Archivo CSV de estadísticas

current_process = None
root = None
output_text = None
btn_next = None

# -------------------------------
# Mostrar texto en la interfaz
# -------------------------------
def append_output(text):
    output_text.config(state=tk.NORMAL)
    output_text.insert(tk.END, text)
    output_text.see(tk.END)
    output_text.config(state=tk.DISABLED)

def clear_output():
    output_text.config(state=tk.NORMAL)
    output_text.delete(1.0, tk.END)
    output_text.config(state=tk.DISABLED)

# -------------------------------
# Ejecutar un comando (C++ o Python)
# -------------------------------
def run_command(command, wait_for_input=False, is_metrics=False):
    global current_process

    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            universal_newlines=True,  # 🔥 necesario en Windows
            bufsize=1
        )
        current_process = process
        append_output(f"\n🚀 Ejecutando: {' '.join(command)}\n")

        for line in process.stdout:
            append_output(line)
            if "Presione ENTER" in line and wait_for_input:
                btn_next.config(state=tk.NORMAL)

        process.wait()
        append_output("\n✅ Ejecución finalizada correctamente.\n")

        # Solo genera métricas si NO estamos ejecutando metrics.py
        if not is_metrics and os.path.exists(CACHE_CSV):
            append_output("📊 Generando métricas y gráficas...\n")
            run_metrics()

    except Exception as e:
        append_output(f"\n❌ Error al ejecutar: {e}\n")

# -------------------------------
# Ejecutar script de métricas
# -------------------------------
def run_metrics():
    run_command(["python", METRICS_SCRIPT], is_metrics=True)
    show_graphs()

# -------------------------------
# Mostrar las gráficas generadas
# -------------------------------
def show_graphs():
    if not os.path.exists("metrics_by_PE.png"):
        append_output("⚠️ No se encontró metrics_by_PE.png\n")
        return

    img1 = Image.open("metrics_by_PE.png").resize((600, 400))
    tk_img1 = ImageTk.PhotoImage(img1)

    win = tk.Toplevel(root)
    win.title("Gráficas de métricas MESI")

    lbl1 = tk.Label(win, image=tk_img1)
    lbl1.image = tk_img1
    lbl1.pack(padx=10, pady=10)

    if os.path.exists("mesi_transitions.png"):
        img2 = Image.open("mesi_transitions.png").resize((600, 400))
        tk_img2 = ImageTk.PhotoImage(img2)
        lbl2 = tk.Label(win, image=tk_img2)
        lbl2.image = tk_img2
        lbl2.pack(padx=10, pady=10)

# -------------------------------
# Función para enviar ENTER al programa (modo stepping)
# -------------------------------
def send_next():
    global current_process
    if current_process and current_process.stdin:
        try:
            current_process.stdin.write("\n")
            current_process.stdin.flush()
            append_output("\n⏭ Paso siguiente enviado.\n")
            btn_next.config(state=tk.DISABLED)
        except Exception as e:
            append_output(f"\n⚠️ Error al enviar ENTER: {e}\n")

# -------------------------------
# Funciones para botones principales
# -------------------------------
def run_dot_mode():
    clear_output()
    Thread(target=lambda: run_command([EXECUTABLE, "--mode=dot", "--N=20"]), daemon=True).start()

def run_stepping_mode():
    clear_output()
    Thread(target=lambda: run_command([EXECUTABLE, "--mode=demo", "--N=20"], wait_for_input=True), daemon=True).start()

# -------------------------------
# Construcción de la interfaz Tkinter
# -------------------------------
def build_gui():
    global root, output_text, btn_next
    root = tk.Tk()
    root.title("Simulador MESI - Producto Punto / Stepping")
    root.geometry("1000x700")
    root.configure(bg="#E6F0FA")

    # -------------------------------
    # Encabezado
    # -------------------------------
    title_frame = tk.Frame(root, bg="#E6F0FA")
    title_frame.pack(pady=15)

    title_label = tk.Label(
        title_frame,
        text="Modelado de un sistema MultiProcesador (MP) con coherencia de cache MESI\npara el cálculo paralelo de producto punto",
        font=("Segoe UI", 14, "bold"),
        bg="#E6F0FA",
        fg="#0B3D91",
        justify="center"
    )
    title_label.pack()

    subtitle_label = tk.Label(
        title_frame,
        text="Integrantes: Ashley Vásquez • Yaritza López • Olman Rodríguez",
        font=("Segoe UI", 11, "italic"),
        bg="#E6F0FA",
        fg="#1A73E8"
    )
    subtitle_label.pack(pady=(5, 15))

    # -------------------------------
    # Botones superiores
    # -------------------------------
    frame_buttons = tk.Frame(root, bg="#E6F0FA")
    frame_buttons.pack(pady=10)

    button_style = {
        "font": ("Segoe UI", 10, "bold"),
        "bg": "#1A73E8",
        "fg": "white",
        "activebackground": "#0B57D0",
        "activeforeground": "white",
        "relief": tk.FLAT,
        "width": 22,
        "cursor": "hand2"
    }

    btn_dot = tk.Button(frame_buttons, text="▶ Modo Producto Punto", command=run_dot_mode, **button_style)
    btn_dot.grid(row=0, column=0, padx=10, pady=5)

    btn_step = tk.Button(frame_buttons, text="🔁 Modo Stepping MESI", command=run_stepping_mode, **button_style)
    btn_step.grid(row=0, column=1, padx=10, pady=5)

    btn_next = tk.Button(frame_buttons, text="⏭ Siguiente evento", command=send_next, state=tk.DISABLED, **button_style)
    btn_next.grid(row=0, column=2, padx=10, pady=5)


    # -------------------------------
    # Consola de salida
    # -------------------------------
    output_frame = tk.Frame(root, bg="#D9E9FF", bd=2, relief="groove")
    output_frame.pack(fill=tk.BOTH, expand=True, padx=20, pady=20)

    output_text = tk.Text(output_frame, wrap=tk.WORD, state=tk.DISABLED, bg="#F8FBFF", fg="#002244", font=("Consolas", 10))
    output_text.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

    root.mainloop()

# -------------------------------
# Ejecución principal
# -------------------------------
if __name__ == "__main__":
    build_gui()
