import pandas as pd
import matplotlib.pyplot as plt

# ---------- Carga ----------
df = pd.read_csv("cache_stats.csv")

# Asegurar tipos numéricos en columnas métricas
metrics_cols = ["Loads","Stores","RW_Accesses","Cache_Misses","Invalidations","BusRd","BusRdX","BusUpgr","Flush"]
for c in metrics_cols:
    if c in df.columns:
        df[c] = pd.to_numeric(df[c], errors="coerce").fillna(0).astype(int)

# ---------- Gráfica 1: Métricas por PE (barras agrupadas) ----------
pe_labels = df["PE"].astype(str).tolist()
x = range(len(df))
bar_width = 0.1

plt.figure(figsize=(10,6))
for i, col in enumerate(metrics_cols):
    offs = [p + i*bar_width for p in x]
    plt.bar(offs, df[col].tolist(), width=bar_width, label=col)

# Centrar los ticks bajo el grupo de barras
center = [p + bar_width*(len(metrics_cols)-1)/2 for p in x]
plt.xticks(center, pe_labels)

plt.xlabel("PE")
plt.ylabel("Cuenta")
plt.title("Métricas por PE")
plt.legend(bbox_to_anchor=(1.02, 1), loc='upper left')
plt.tight_layout()
plt.savefig("metrics_by_PE.png")
plt.close()

# ---------- Gráfica 2: Transiciones MESI totales ----------
# Tu CSV trae algo como:
# "MESI: 2->1; MESI: 1->0; MESI: 1->3; MESI: 3->1"
# No es una lista Python, así que NO usamos ast.literal_eval.

transition_counts = {}

def add_transition(t: str):
    t = t.strip()
    if not t:
        return
    # Normaliza espacios y separadores
    # Opcional: quitar prefijo "MESI:" si aparece
    if t.lower().startswith("mesi:"):
        t = t[5:].strip()
    transition_counts[t] = transition_counts.get(t, 0) + 1

if "Transitions" in df.columns:
    for cell in df["Transitions"].fillna(""):
        # Puede venir vacío o con múltiples eventos separados por ';'
        parts = [p for p in str(cell).split(";") if p.strip()]
        for p in parts:
            add_transition(p)

# Si no hay transiciones, evitamos crashear
if transition_counts:
    plt.figure(figsize=(10,6))
    keys = list(transition_counts.keys())
    values = [transition_counts[k] for k in keys]
    plt.bar(keys, values)
    plt.xlabel("Transición MESI")
    plt.ylabel("Cantidad")
    plt.title("Transiciones MESI Totales")
    plt.xticks(rotation=45, ha="right")
    plt.tight_layout()
    plt.savefig("mesi_transitions.png")
    plt.close()
else:
    print("No hay transiciones MESI registradas en el CSV.")

print("Listo: metrics_by_PE.png y (si aplica) mesi_transitions.png")

