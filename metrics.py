import pandas as pd
import matplotlib.pyplot as plt
import ast

# Leer CSV
df = pd.read_csv("cache_stats.csv")

# ---------------------------------------------
# Gráfica 1: Métricas por PE (todas en un gráfico)
# ---------------------------------------------
metrics_cols = ["Loads","Stores","RW_Accesses","Cache_Misses","Invalidations","BusRd","BusRdX","BusUpgr","Flush"]
pe_indices = df["PE"].astype(str)

# Configurar tamaño del gráfico
plt.figure(figsize=(10,6))

# Para cada métrica, dibujar barras agrupadas
bar_width = 0.1
x = range(len(df))
for i, col in enumerate(metrics_cols):
    plt.bar([p + i*bar_width for p in x], df[col], width=bar_width, label=col)

plt.xticks([p + bar_width*4 for p in x], pe_indices)
plt.xlabel("PE")
plt.ylabel("Count")
plt.title("Métricas por PE")
plt.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
plt.tight_layout()
plt.savefig("metrics_by_PE.png")
plt.show()

# ---------------------------------------------
# Gráfica 2: Transiciones MESI
# ---------------------------------------------
plt.figure(figsize=(10,6))

# Contar las transiciones de todas las PEs
transition_counts = {}
for tlist in df["Transitions"]:
    # Convertir string de lista a lista real
    tlist_parsed = ast.literal_eval(tlist)
    for t in tlist_parsed:
        if t in transition_counts:
            transition_counts[t] += 1
        else:
            transition_counts[t] = 1

# Graficar
plt.bar(transition_counts.keys(), transition_counts.values(), color='skyblue')
plt.xlabel("Transición MESI")
plt.ylabel("Cantidad")
plt.title("Transiciones MESI Totales")
plt.xticks(rotation=45)
plt.tight_layout()
plt.savefig("mesi_transitions.png")
plt.show()
