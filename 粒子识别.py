#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
multi_tif_viewer2.py (v2) + 批量输出优化 + 方形网格支持 + 网格灰度和显示
新增功能：
- 可以选择网格形状：圆网格（原有）或正方形网格（边长 = 圆网格直径）
- 当选择显示正方形网格后，所有基于“圆网格内”的检测/统计操作会改为“正方形网格内”进行
- 增加“累加网格内灰度值并以每行一条的形式显示（方便复制）”的功能，显示形式为：
    A1 100
    A2 500
    A3 200
  并提供“复制到剪贴板”按钮
- 新增：保存统计网格总计数为 .tif 图像（在图像上绘制网格与每格的编号和统计值，例如 A1：1000）
依赖：
pip install numpy pillow scikit-image scipy tifffile
"""
import os
import tkinter as tk
from tkinter import filedialog, Frame, Label, Entry, Button, messagebox
from PIL import Image, ImageTk
import numpy as np

import tifffile as tff
from skimage import filters, morphology, measure, util, segmentation, feature
from scipy import ndimage as ndi
from scipy.spatial import cKDTree


# ============================================================
# Helpers (原有)
# ============================================================
def window_level(img_arr, window_center, window_width, threshold=None, invert=False):
    wl = int(window_center)
    ww = int(window_width)
    lower = wl - ww // 2
    upper = wl + ww // 2
    arr = img_arr.copy()
    arr = np.clip(arr, lower, upper)
    arr = ((arr - lower) * 65535.0 / max(ww, 1)).astype(np.uint16)
    if threshold is not None:
        arr[img_arr > threshold] = 0
    if invert:
        arr = 65535 - arr
    return (arr / 256).astype(np.uint8)


def index_to_letters(idx: int) -> str:
    s = ""
    n = idx
    while True:
        s = chr(ord('A') + (n % 26)) + s
        n = n // 26 - 1
        if n < 0:
            break
    return s


def hsv_to_rgb(h, s, v):
    i = int(h * 6.0)
    f = (h * 6.0) - i
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))
    i = i % 6
    if i == 0:
        r, g, b = v, t, p
    elif i == 1:
        r, g, b = q, v, p
    elif i == 2:
        r, g, b = p, v, t
    elif i == 3:
        r, g, b = p, q, v
    elif i == 4:
        r, g, b = t, p, v
    else:
        r, g, b = v, p, q
    return int(r * 255), int(g * 255), int(b * 255)


# ============================================================
# Batch helpers (新增)
# ============================================================
def read_tif_as_array(path: str) -> np.ndarray:
    """
    批处理读取 tif/tiff：
    - 优先 tifffile（多页取第 1 页）
    - 失败回退 PIL
    返回：2D numpy 数组
    """
    try:
        arr = tff.imread(path)
        if arr is None:
            raise ValueError("tifffile.imread returned None")
        if arr.ndim >= 3:
            arr = arr[0]
        if arr.ndim != 2:
            raise ValueError(f"Unexpected shape: {arr.shape}")
        return np.array(arr)
    except Exception:
        img = Image.open(path)
        if img.mode not in ('L', 'I;16'):
            try:
                img = img.convert('I;16')
            except Exception:
                img = img.convert('L')
        return np.array(img)


def circle_label_order(rows: int, cols: int):
    order = []
    for ri in range(rows):
        for ci in range(cols):
            order.append(f"{index_to_letters(ri)}{ci+1}")
    return order


def save_text_overwrite(path: str, text: str):
    """
    强制覆盖写入文本文件（同名直接覆盖）。
    """
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def save_per_image_counts_txt(img_path: str, circle_regions, total_count: int, detect_params: dict, circle_params: dict) -> str:
    base, _ = os.path.splitext(img_path)
    out_path = base + ".txt"

    rows = int(circle_params["rows"])
    cols = int(circle_params["cols"])
    offset_x = int(circle_params["offset_x"])
    offset_y = int(circle_params["offset_y"])
    diameter_cm = float(circle_params.get("diameter_cm", 0.9))
    grid_shape = circle_params.get("grid_shape", "circle")

    lines = []
    lines.append(f"image\t{os.path.basename(img_path)}")
    lines.append(f"total_spots\t{int(total_count)}")
    lines.append(f"grid_shape\t{grid_shape}")
    lines.append(f"circle_grid\trows={rows}\tcols={cols}\toffset_x={offset_x}\toffset_y={offset_y}\tdiameter_cm={diameter_cm}")
    lines.append(
        "detection_params\tuse_otsu={}\tmin_pixels={}\tmax_pixels={}".format(
            int(bool(detect_params["use_otsu"])),
            int(detect_params["min_pixels"]),
            int(detect_params["max_pixels"]),
        )
    )
    lines.append("")
    lines.append("circle_label\tspot_count")
    for label, regions in circle_regions:
        lines.append(f"{label}\t{len(regions)}")

    # 覆盖同名文件
    save_text_overwrite(out_path, "\n".join(lines))
    return out_path


def save_folder_summary_txt(folder: str, tif_files: list, label_order: list, sum_counts: dict, detect_params: dict, circle_params: dict) -> str:
    out_path = os.path.join(folder, "folder_big_spot_summary.txt")

    rows = int(circle_params["rows"])
    cols = int(circle_params["cols"])
    offset_x = int(circle_params["offset_x"])
    offset_y = int(circle_params["offset_y"])
    diameter_cm = float(circle_params.get("diameter_cm", 0.9))
    grid_shape = circle_params.get("grid_shape", "circle")

    lines = []
    lines.append("summary_type\tcircle_spot_count_sum_across_images")
    lines.append(f"folder\t{folder}")
    lines.append(f"image_count\t{len(tif_files)}")
    lines.append(f"grid_shape\t{grid_shape}")
    lines.append(f"circle_grid\trows={rows}\tcols={cols}\toffset_x={offset_x}\toffset_y={offset_y}\tdiameter_cm={diameter_cm}")
    lines.append(
        "detection_params\tuse_otsu={}\tmin_pixels={}\tmax_pixels={}".format(
            int(bool(detect_params["use_otsu"])),
            int(detect_params["min_pixels"]),
            int(detect_params["max_pixels"]),
        )
    )
    lines.append("")
    lines.append("circle_label\tsum_spot_count")
    for label in label_order:
        lines.append(f"{label}\t{int(sum_counts.get(label, 0))}")

    # 覆盖同名文件
    save_text_overwrite(out_path, "\n".join(lines))
    return out_path


def build_folder_pixel_sum_saturated_uint16(tif_files: list) -> np.ndarray:
    """
    逐像素叠加（饱和加法）：
    - 任意像素一旦超过 65535，最终固定为 65535（不管超多少）
    - 返回 uint16（可直接保存 raw16.tif）
    """
    sum16 = None
    base_shape = None

    for path in tif_files:
        arr = read_tif_as_array(path)
        if arr.ndim != 2:
            raise ValueError(f"Only 2D images supported: {os.path.basename(path)} shape={arr.shape}")
        if sum16 is None:
            base_shape = arr.shape
            sum16 = np.zeros(base_shape, dtype=np.uint16)
        if arr.shape != base_shape:
            raise ValueError(f"Image shapes differ: {os.path.basename(path)} {arr.shape} != {base_shape}")

        tmp = sum16.astype(np.uint32) + arr.astype(np.uint32)
        np.clip(tmp, 0, 65535, out=tmp)
        sum16 = tmp.astype(np.uint16)

    if sum16 is None:
        raise ValueError("No images to sum.")
    return sum16


def normalize_uint16_to_uint8(img16: np.ndarray) -> np.ndarray:
    if img16.size == 0:
        return np.zeros((1, 1), dtype=np.uint8)
    vmax = float(np.max(img16))
    if vmax <= 0:
        return np.zeros_like(img16, dtype=np.uint8)
    out = (img16.astype(np.float32) / vmax) * 255.0
    return np.clip(out, 0, 255).astype(np.uint8)


def draw_grid_and_text_on_sum_image(sum16: np.ndarray, circle_params: dict, sum_counts: dict) -> Image.Image:
    """
    在叠加图可视化上画网格（圆或方），并在每个格中心写 "A1：203"。
    输出 PIL RGB 图像。
    circle_params 中包含 grid_shape='circle' 或 'square'
    """
    from PIL import ImageDraw, ImageFont

    h, w = sum16.shape
    gray8 = normalize_uint16_to_uint8(sum16)
    pil_img = Image.fromarray(gray8, mode="L").convert("RGB")
    draw = ImageDraw.Draw(pil_img)

    rows = int(circle_params["rows"])
    cols = int(circle_params["cols"])
    offset_x = int(circle_params["offset_x"])
    offset_y = int(circle_params["offset_y"])
    diameter_px_raw = float(circle_params["diameter_px_raw"])
    r = diameter_px_raw / 2.0
    grid_shape = circle_params.get("grid_shape", "circle")

    grid_w = diameter_px_raw * cols
    grid_h = diameter_px_raw * rows
    start_x = (w - grid_w) / 2.0 + offset_x
    start_y = (h - grid_h) / 2.0 + offset_y

    try:
        font = ImageFont.truetype("msyh.ttc", size=18)
    except Exception:
        try:
            font = ImageFont.truetype("PingFang.ttc", size=18)
        except Exception:
            try:
                font = ImageFont.truetype("NotoSansCJK-Regular.ttc", size=18)
            except Exception:
                font = ImageFont.load_default()

    circle_outline = (255, 0, 0)
    rect_outline = (255, 128, 0)
    text_fill = (255, 255, 0)
    text_stroke = (0, 0, 0)

    for ri in range(rows):
        for ci in range(cols):
            label = f"{index_to_letters(ri)}{ci+1}"
            cx = start_x + (ci + 0.5) * diameter_px_raw
            cy = start_y + (ri + 0.5) * diameter_px_raw

            left, top = cx - r, cy - r
            right, bottom = cx + r, cy + r
            if right <= 0 or bottom <= 0 or left >= w or top >= h:
                continue

            if grid_shape == 'circle':
                draw.ellipse([left, top, right, bottom], outline=circle_outline, width=2)
            else:
                # square: draw rectangle with same side as diameter
                draw.rectangle([left, top, right, bottom], outline=rect_outline, width=2)

            cnt = int(sum_counts.get(label, 0))
            text = f"{label}：{cnt}"

            try:
                bbox = draw.textbbox((0, 0), text, font=font, stroke_width=2)
                tw = bbox[2] - bbox[0]
                th = bbox[3] - bbox[1]
            except Exception:
                tw, th = draw.textsize(text, font=font)

            tx = cx - tw / 2.0
            ty = cy - th / 2.0
            try:
                draw.text((tx, ty), text, fill=text_fill, font=font, stroke_width=2, stroke_fill=text_stroke)
            except Exception:
                draw.text((tx, ty), text, fill=text_fill, font=font)

    return pil_img


def save_folder_pixel_sum_double_outputs(folder: str, sum16: np.ndarray, overlay_rgb: Image.Image) -> tuple[str, str]:
    """
    双输出保存（覆盖同名文件）：
    - folder_pixel_sum_raw16.tif：sum16（饱和叠加结果）
    - folder_pixel_sum_overlay_vis.tif：overlay_rgb（可视化）
    """
    raw16_path = os.path.join(folder, "folder_pixel_sum_raw16.tif")
    vis_path = os.path.join(folder, "folder_pixel_sum_overlay_vis.tif")

    # tifffile.imwrite 默认覆盖同名文件
    tff.imwrite(raw16_path, sum16.astype(np.uint16))

    vis_arr = np.array(overlay_rgb).astype(np.uint8)
    tff.imwrite(vis_path, vis_arr)

    return raw16_path, vis_path


# ============================================================
# SimpleRegion (原有)
# ============================================================
class SimpleRegion:
    def __init__(self, pixel_count, centroid, coords):
        self.area = pixel_count
        self.centroid = centroid
        self.coords = coords
        self.color_idx = None


# ============================================================
# ImagePanel (v2：按网格检测 + 着色overlay)
# ============================================================
class ImagePanel(Frame):
    def __init__(self, master, img_arr, img_name, remove_callback, img_path=None, select_callback=None):
        super().__init__(master, bd=2, relief=tk.GROOVE)
        self.img_arr = img_arr
        self.img_name = img_name
        self.remove_callback = remove_callback

        # v2新增：路径与选中回调（用于批量）
        self.img_path = img_path
        self.select_callback = select_callback
        self.is_selected = False

        # display / interaction
        self.photo = None
        self.scale = 1.0
        self.image_pos = [0.0, 0.0]
        self.user_zoom = False
        self.min_scale = 0.1
        self.max_scale = 8.0

        # display params
        self.invert_var = tk.BooleanVar(value=False)
        minv, maxv = int(img_arr.min()), int(img_arr.max())
        self.window_level_var = tk.IntVar(value=max(1, (minv + maxv) // 2))
        self.window_width_var = tk.IntVar(value=max(2, max(1, maxv - minv)))
        self.threshold_var = tk.IntVar(value=int(img_arr.max()))

        # grid / misc
        # show_grid_var: whether to show either circle or square grid (depending on grid_shape_var)
        self.show_grid_var = tk.BooleanVar(value=False)
        self.grid_shape_var = tk.StringVar(value='circle')  # 'circle' or 'square'
        self.circle_rows_var = tk.IntVar(value=8)
        self.circle_cols_var = tk.IntVar(value=12)
        # NOTE: offset_x/offset_y are interpreted as ORIGINAL IMAGE PIXELS.
        # Changing window size / zoom will not change the grid's position relative to the image content.
        self.circle_offset_x = 0
        self.circle_offset_y = 0

        self.show_rectgrid_var = tk.BooleanVar(value=False)
        self.rect_rows_var = tk.IntVar(value=4)
        self.rect_cols_var = tk.IntVar(value=6)
        self.rect_offset_x = 0
        self.rect_offset_y = 0
        self.rect_cell_px = 416

        # detection results & overlay
        self.detected_regions = []
        self.annotated_overlay = None
        self.show_big_spots_var = tk.BooleanVar(value=True)

        # regions per grid cell
        self.circle_regions = []

        # PIL display
        self.orig_pil = None
        # orig_w / orig_h store original image size in pixels
        self.orig_w = 0
        self.orig_h = 0

        self._create_widgets()
        self.update_image_display()

    # -------- v2新增：选中高亮 --------
    def set_selected(self, selected: bool):
        self.is_selected = selected
        if selected:
            self.config(highlightthickness=3, highlightbackground="#1e90ff", highlightcolor="#1e90ff")
        else:
            self.config(highlightthickness=0)

    def _on_click_select(self, event=None):
        if callable(self.select_callback):
            self.select_callback(self)

    def _create_widgets(self):
        # Canvas
        self.canvas = tk.Canvas(self, width=300, height=300, bg='grey', highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", self.on_resize)
        self.canvas.bind("<Motion>", self.show_pixel_info)
        self.canvas.bind("<MouseWheel>", self.on_mousewheel)
        self.canvas.bind("<Button-4>", self.on_mousewheel_linux)
        self.canvas.bind("<Button-5>", self.on_mousewheel_linux)

        # 点击选中
        self.bind("<Button-1>", self._on_click_select, add="+")
        self.canvas.bind("<Button-1>", self._on_click_select, add="+")

        # Top bar
        top_frame = Frame(self)
        Label(top_frame, text=self.img_name, anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True)
        Button(top_frame, text="关闭", command=self.close_panel).pack(side=tk.RIGHT)
        self.toggle_func_btn = Button(top_frame, text="隐藏功能区", command=self.toggle_function_area)
        self.toggle_func_btn.pack(side=tk.RIGHT)
        top_frame.pack(fill=tk.X)

        # Function area
        self.function_area = Frame(self)
        self.function_area.pack(fill=tk.X, pady=2)

        # WL/WW
        maxv_safe = max(1, int(self.img_arr.max()))
        self.window_level_slider = tk.Scale(
            self.function_area, from_=1, to=maxv_safe, orient=tk.HORIZONTAL,
            label="窗口中心", variable=self.window_level_var,
            command=lambda v: self.update_image_display()
        )
        self.window_level_slider.pack(fill=tk.X)

        safe_maxww = max(2, int(self.img_arr.max() - self.img_arr.min()))
        self.window_width_slider = tk.Scale(
            self.function_area, from_=2, to=safe_maxww, orient=tk.HORIZONTAL,
            label="窗口宽度", variable=self.window_width_var,
            command=lambda v: self.update_image_display()
        )
        self.window_width_slider.pack(fill=tk.X)

        # Threshold
        threshold_frame = Frame(self.function_area)
        Label(threshold_frame, text="显示时隐藏 > ").pack(side=tk.LEFT)
        self.threshold_entry = Entry(threshold_frame, width=8)
        self.threshold_entry.insert(0, str(self.threshold_var.get()))
        self.threshold_entry.pack(side=tk.LEFT)
        Label(threshold_frame, text=" 的像素").pack(side=tk.LEFT)
        Button(threshold_frame, text="应用", command=self.apply_threshold).pack(side=tk.LEFT)
        threshold_frame.pack(fill=tk.X, pady=2)

        # Invert
        invert_frame = Frame(self.function_area)
        self.invert_check = tk.Checkbutton(invert_frame, text="黑白反转", variable=self.invert_var, command=self.update_image_display)
        self.invert_check.pack(side=tk.LEFT)
        invert_frame.pack(fill=tk.X, pady=2)

        # circle/grid controls
        circle_frame = Frame(self.function_area)
        # show grid checkbox (circle or square depends on grid_shape_var)
        self.circle_check = tk.Checkbutton(circle_frame, text="显示 网格 (圆/方) (φ=0.9cm)", variable=self.show_grid_var, command=self.update_image_display)
        self.circle_check.grid(row=0, column=0, columnspan=6, sticky='w')

        # grid shape radio buttons
        Label(circle_frame, text="网格形状:").grid(row=0, column=6, sticky='e')
        tk.Radiobutton(circle_frame, text='圆', variable=self.grid_shape_var, value='circle', command=self.update_image_display).grid(row=0, column=7, sticky='w')
        tk.Radiobutton(circle_frame, text='方', variable=self.grid_shape_var, value='square', command=self.update_image_display).grid(row=0, column=8, sticky='w')

        Label(circle_frame, text="行数:").grid(row=1, column=0, sticky='e')
        self.circle_rows_entry = Entry(circle_frame, width=5)
        self.circle_rows_entry.insert(0, str(self.circle_rows_var.get()))
        self.circle_rows_entry.grid(row=1, column=1, sticky='w', padx=(2, 6))

        Label(circle_frame, text="列数:").grid(row=1, column=2, sticky='e')
        self.circle_cols_entry = Entry(circle_frame, width=5)
        self.circle_cols_entry.insert(0, str(self.circle_cols_var.get()))
        self.circle_cols_entry.grid(row=1, column=3, sticky='w', padx=(2, 6))

        Label(circle_frame, text="偏移X (px):").grid(row=1, column=4, sticky='e')
        self.circle_offset_x_entry = Entry(circle_frame, width=6)
        self.circle_offset_x_entry.insert(0, "0")
        self.circle_offset_x_entry.grid(row=1, column=5, sticky='w', padx=(2, 6))

        Label(circle_frame, text="偏移Y (px):").grid(row=1, column=6, sticky='e')
        self.circle_offset_y_entry = Entry(circle_frame, width=6)
        self.circle_offset_y_entry.insert(0, "0")
        self.circle_offset_y_entry.grid(row=1, column=7, sticky='w', padx=(2, 6))

        Label(circle_frame, text="统计忽略 > ").grid(row=1, column=8, sticky='e')
        self.stats_upper_entry = Entry(circle_frame, width=8)
        self.stats_upper_entry.insert(0, str(int(self.img_arr.max())))
        self.stats_upper_entry.grid(row=1, column=9, sticky='w', padx=(2, 6))

        Label(circle_frame, text=" 的像素").grid(row=1, column=10, sticky='w')
        Button(circle_frame, text="应用网格参数", command=self.apply_circle_params).grid(row=1, column=11, padx=6)
        Button(circle_frame, text="统计网格总计数", command=self.compute_circle_stats).grid(row=1, column=12, padx=6)
        # 新增：统计网格灰度和（每行一条，便于复制）
        Button(circle_frame, text="统计网格灰度和（每行一条）", command=self.compute_grid_pixel_sum_lines).grid(row=1, column=13, padx=6)
        # 新增：保存网格统计为 tif（在图像上绘制网格及每格编号与总计数）
        Button(circle_frame, text="保存网格统计为TIF", command=self.save_grid_stats_as_tif).grid(row=1, column=14, padx=6)
        circle_frame.pack(fill=tk.X, pady=2)

        # rect grid
        rect_frame = Frame(self.function_area)
        self.rect_check = tk.Checkbutton(rect_frame, text="显示 矩形网格（单元 416×416 原始像素）", variable=self.show_rectgrid_var, command=self.update_image_display)
        self.rect_check.grid(row=0, column=0, columnspan=10, sticky='w')

        Label(rect_frame, text="行数:").grid(row=1, column=0, sticky='e')
        self.rect_rows_entry = Entry(rect_frame, width=5)
        self.rect_rows_entry.insert(0, str(self.rect_rows_var.get()))
        self.rect_rows_entry.grid(row=1, column=1, sticky='w', padx=(2, 6))

        Label(rect_frame, text="列数:").grid(row=1, column=2, sticky='e')
        self.rect_cols_entry = Entry(rect_frame, width=5)
        self.rect_cols_entry.insert(0, str(self.rect_cols_var.get()))
        self.rect_cols_entry.grid(row=1, column=3, sticky='w', padx=(2, 6))

        Label(rect_frame, text="偏移X (px):").grid(row=1, column=4, sticky='e')
        self.rect_offset_x_entry = Entry(rect_frame, width=6)
        self.rect_offset_x_entry.insert(0, "0")
        self.rect_offset_x_entry.grid(row=1, column=5, sticky='w', padx=(2, 6))

        Label(rect_frame, text="偏移Y (px):").grid(row=1, column=6, sticky='e')
        self.rect_offset_y_entry = Entry(rect_frame, width=6)
        self.rect_offset_y_entry.insert(0, "0")
        self.rect_offset_y_entry.grid(row=1, column=7, sticky='w', padx=(2, 6))

        Button(rect_frame, text="应用矩形网格参数", command=self.apply_rect_params).grid(row=1, column=8, padx=6)
        rect_frame.pack(fill=tk.X, pady=2)

        # Detection controls
        detect_frame = Frame(self.function_area)
        Label(detect_frame, text="检测：").grid(row=0, column=0, sticky='w')

        Label(detect_frame, text="use_otsu:").grid(row=0, column=1, sticky='e')
        self.use_otsu_var = tk.BooleanVar(value=False)
        tk.Checkbutton(detect_frame, variable=self.use_otsu_var).grid(row=0, column=2, sticky='w', padx=(0, 8))

        Label(detect_frame, text="min_pixels:").grid(row=0, column=3, sticky='e')
        self.min_pixels_entry = Entry(detect_frame, width=6)
        self.min_pixels_entry.insert(0, "5")
        self.min_pixels_entry.grid(row=0, column=4, sticky='w', padx=(2, 8))

        Label(detect_frame, text="max_pixels (0 不限制):").grid(row=0, column=5, sticky='e')
        self.max_pixels_entry = Entry(detect_frame, width=6)
        self.max_pixels_entry.insert(0, "10")
        self.max_pixels_entry.grid(row=0, column=6, sticky='w', padx=(2, 8))

        self.show_big_spots_check = tk.Checkbutton(detect_frame, text="显示大亮点", variable=self.show_big_spots_var, command=self._update_overlay_visibility)
        self.show_big_spots_check.grid(row=0, column=7, sticky='w', padx=(6, 8))

        Button(detect_frame, text="检测并标注大亮点", command=self.run_detection).grid(row=0, column=8, padx=(8, 6))
        Button(detect_frame, text="保存���注图", command=self.save_annotated_image).grid(row=0, column=9, padx=(8, 6))
        detect_frame.pack(fill=tk.X, pady=2)

    def toggle_function_area(self):
        if self.function_area.winfo_ismapped():
            self.function_area.pack_forget()
            self.toggle_func_btn.config(text="显示功能区")
        else:
            self.function_area.pack(fill=tk.X, pady=2)
            self.toggle_func_btn.config(text="隐藏功能区")

    def apply_circle_params(self):
        try:
            rows = int(self.circle_rows_entry.get())
            cols = int(self.circle_cols_entry.get())
            ox = int(self.circle_offset_x_entry.get())
            oy = int(self.circle_offset_y_entry.get())
            rows = max(1, min(200, rows))
            cols = max(1, min(200, cols))
            self.circle_rows_var.set(rows)
            self.circle_cols_var.set(cols)
            # store offsets as original-image pixels
            self.circle_offset_x = ox
            self.circle_offset_y = oy
            self.update_image_display()
        except Exception:
            pass

    def apply_rect_params(self):
        try:
            rows = int(self.rect_rows_entry.get())
            cols = int(self.rect_cols_entry.get())
            ox = int(self.rect_offset_x_entry.get())
            oy = int(self.rect_offset_y_entry.get())
            rows = max(1, min(200, rows))
            cols = max(1, min(200, cols))
            self.rect_rows_var.set(rows)
            self.rect_cols_var.set(cols)
            self.rect_offset_x = ox
            self.rect_offset_y = oy
            self.update_image_display()
        except Exception:
            pass

    def apply_threshold(self):
        try:
            thr = int(self.threshold_entry.get())
            self.threshold_var.set(thr)
            self.update_image_display()
        except Exception:
            pass

    def _update_overlay_visibility(self):
        if self.show_big_spots_var.get() and self.detected_regions:
            try:
                max_pixels = int(self.max_pixels_entry.get())
            except Exception:
                max_pixels = 0
            self.annotated_overlay = self._build_colored_overlay(self.detected_regions, max_pixels=max_pixels)
        else:
            self.annotated_overlay = None
        self.update_image_display()

    def _generate_palette(self, n):
        palette = []
        for i in range(n):
            h = float(i) / max(1, n)
            s = 0.7
            v = 0.95
            palette.append(hsv_to_rgb(h, s, v))
        return palette

    def _assign_colors_greedy(self, regions):
        N = len(regions)
        if N == 0:
            return []
        centroids = np.array([r.centroid for r in regions])
        radii = np.array([np.sqrt(max(1.0, r.area) / np.pi) for r in regions])
        ADJ_FACTOR = 1.2

        pts = np.column_stack((centroids[:, 1], centroids[:, 0]))
        tree = cKDTree(pts)

        neighbor_lists = [set() for _ in range(N)]
        for i in range(N):
            thresh = (radii[i] + radii.max()) * ADJ_FACTOR
            idxs = tree.query_ball_point(pts[i], r=thresh)
            for j in idxs:
                if j == i:
                    continue
                dist = np.hypot(pts[i, 0] - pts[j, 0], pts[i, 1] - pts[j, 1])
                threshold_ij = (radii[i] + radii[j]) * ADJ_FACTOR
                if dist < threshold_ij:
                    neighbor_lists[i].add(j)
                    neighbor_lists[j].add(i)

        degrees = [len(neighbor_lists[i]) for i in range(N)]
        order = sorted(range(N), key=lambda x: -degrees[x])
        color_idx = [-1] * N
        for idx in order:
            used = {color_idx[nbr] for nbr in neighbor_lists[idx] if color_idx[nbr] != -1}
            c = 0
            while c in used:
                c += 1
            color_idx[idx] = c

        num_colors_needed = max(color_idx) + 1
        palette = self._generate_palette(max(8, num_colors_needed))
        return [palette[c % len(palette)] for c in color_idx]

    def _build_colored_overlay(self, regions, max_pixels=0, alpha=160):
        if not regions:
            return None
        h, w = self.img_arr.shape if self.img_arr.ndim == 2 else self.img_arr.shape[:2]
        colors_rgb = self._assign_colors_greedy(regions)
        overlay_arr = np.zeros((h, w, 4), dtype=np.uint8)
        for r, col in zip(regions, colors_rgb):
            coords = getattr(r, "coords", None)
            if coords is None or len(coords) == 0:
                continue
            rr = coords[:, 0].astype(np.int32)
            cc = coords[:, 1].astype(np.int32)
            rr = np.clip(rr, 0, h - 1)
            cc = np.clip(cc, 0, w - 1)
            overlay_arr[rr, cc, 0] = col[0]
            overlay_arr[rr, cc, 1] = col[1]
            overlay_arr[rr, cc, 2] = col[2]
            overlay_arr[rr, cc, 3] = alpha
        try:
            return Image.fromarray(overlay_arr, mode='RGBA')
        except Exception:
            return Image.new('RGBA', (w, h), (0, 0, 0, 0))

    def on_mousewheel(self, event):
        ctrl_pressed = (event.state & 0x4) != 0
        if not ctrl_pressed:
            return
        factor = 1.15 if event.delta > 0 else 1 / 1.15
        canvas_x, canvas_y = event.x, event.y
        img_x = (canvas_x - self.image_pos[0]) / max(self.scale, 1e-9)
        img_y = (canvas_y - self.image_pos[1]) / max(self.scale, 1e-9)
        new_scale = max(self.min_scale, min(self.max_scale, self.scale * factor))
        new_canvas_x = img_x * new_scale
        new_canvas_y = img_y * new_scale
        self.image_pos[0] = canvas_x - new_canvas_x
        self.image_pos[1] = canvas_y - new_canvas_y
        self.scale = new_scale
        self.user_zoom = True
        self.redraw_image()

    def on_mousewheel_linux(self, event):
        ctrl_pressed = (event.state & 0x4) != 0
        if not ctrl_pressed:
            return
        factor = 1.15 if event.num == 4 else 1 / 1.15
        canvas_x, canvas_y = event.x, event.y
        img_x = (canvas_x - self.image_pos[0]) / max(self.scale, 1e-9)
        img_y = (canvas_y - self.image_pos[1]) / max(self.scale, 1e-9)
        new_scale = max(self.min_scale, min(self.max_scale, self.scale * factor))
        new_canvas_x = img_x * new_scale
        new_canvas_y = img_y * new_scale
        self.image_pos[0] = canvas_x - new_canvas_x
        self.image_pos[1] = canvas_y - new_canvas_y
        self.scale = new_scale
        self.user_zoom = True
        self.redraw_image()

    def redraw_image(self):
        if self.orig_pil is None:
            return
        disp_w = max(1, int(round(self.orig_w * self.scale)))
        disp_h = max(1, int(round(self.orig_h * self.scale)))
        img_disp = self.orig_pil.resize((disp_w, disp_h), Image.NEAREST)
        if img_disp.mode != 'RGBA':
            img_disp = img_disp.convert('RGBA')

        if self.annotated_overlay is not None:
            ann = self.annotated_overlay.resize((disp_w, disp_h), Image.NEAREST).convert('RGBA')
            base = img_disp.convert('RGBA')
            img_disp = Image.alpha_composite(base, ann)

        # draw grid (circle or square) based on ORIGINAL image coordinates mapped to display coords
        if self.show_grid_var.get():
            img_disp = self._draw_grid_on_image(
                img_disp,
                rows=self.circle_rows_var.get(),
                cols=self.circle_cols_var.get(),
                diameter_cm=0.9,
                offset_x=self.circle_offset_x,
                offset_y=self.circle_offset_y,
                grid_shape=self.grid_shape_var.get()
            )
        if self.show_rectgrid_var.get():
            img_disp = self._draw_rect_grid_on_image(
                img_disp,
                rows=self.rect_rows_var.get(),
                cols=self.rect_cols_var.get(),
                cell_px=self.rect_cell_px,
                offset_x=self.rect_offset_x,
                offset_y=self.rect_offset_y
            )

        self.photo = ImageTk.PhotoImage(img_disp)
        self.canvas.delete('IMG')
        self.canvas.create_image(int(round(self.image_pos[0])), int(round(self.image_pos[1])), anchor=tk.NW, image=self.photo, tags='IMG')
        x0, y0 = int(round(self.image_pos[0])), int(round(self.image_pos[1]))
        x1, y1 = x0 + disp_w, y0 + disp_h
        self.canvas.config(scrollregion=(min(0, x0), min(0, y0), max(self.canvas.winfo_width(), x1), max(self.canvas.winfo_height(), y1)))

    def update_image_display(self):
        wl = self.window_level_var.get()
        ww = self.window_width_var.get()
        threshold = self.threshold_var.get()
        invert = self.invert_var.get()
        arr_disp = window_level(self.img_arr, wl, ww, threshold, invert)
        self.orig_pil = Image.fromarray(arr_disp)
        # orig_pil.size -> (width, height)
        self.orig_w, self.orig_h = self.orig_pil.size

        canvas_w = self.canvas.winfo_width()
        canvas_h = self.canvas.winfo_height()
        if canvas_w <= 1 or canvas_h <= 1:
            fit_scale = 1.0
        else:
            fit_scale = min(canvas_w / max(self.orig_w, 1), canvas_h / max(self.orig_h, 1))
            fit_scale = min(fit_scale, 1.0)
        if not self.user_zoom:
            self.scale = fit_scale
            disp_w = int(round(self.orig_w * self.scale))
            disp_h = int(round(self.orig_h * self.scale))
            self.image_pos[0] = max(0, (canvas_w - disp_w) / 2.0)
            self.image_pos[1] = max(0, (canvas_h - disp_h) / 2.0)

        self.redraw_image()

    def _draw_grid_on_image(self, pil_img, rows=8, cols=12, diameter_cm=0.9, offset_x=0, offset_y=0, grid_shape='circle'):
        """
        在 display 图像（已按 self.scale 缩放）上绘制网格（圆或方），计算基于原始图像坐标：
        - offset_x / offset_y 为原始像素单位
        - 仅在修改 offset_x/offset_y 或图像内容时网格相对于图像内容发生实际移动
        - 窗口缩放/平移仅影响显示（位置按缩放量同步），但网格相对于图像像素位置不变
        """
        from PIL import ImageDraw
        draw = ImageDraw.Draw(pil_img, 'RGBA')
        disp_w, disp_h = pil_img.size
        # original diameter in pixels
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm

        # compute original-image coordinates for grid origin (top-left of grid)
        grid_w_orig = diameter_px_raw * cols
        grid_h_orig = diameter_px_raw * rows
        start_x_orig = (self.orig_w - grid_w_orig) / 2.0 + offset_x
        start_y_orig = (self.orig_h - grid_h_orig) / 2.0 + offset_y

        # map to display coordinates by scaling
        display_diameter = diameter_px_raw * self.scale
        # do not auto-resize to fit; preserve actual positions
        display_diameter = max(1.0, display_diameter)
        start_x_disp = start_x_orig * self.scale
        start_y_disp = start_y_orig * self.scale

        r = display_diameter / 2.0
        outline_width = max(1, min(4, int(round(display_diameter * 0.01))))
        outline_color_circle = (200, 0, 0, 220)
        outline_color_square = (200, 120, 0, 220)
        for ri in range(rows):
            cy = start_y_disp + (ri + 0.5) * display_diameter
            for ci in range(cols):
                cx = start_x_disp + (ci + 0.5) * display_diameter
                left, top = cx - r, cy - r
                right, bottom = cx + r, cy + r
                # drawing on PIL image: skip if completely outside visible display image
                if right <= 0 or bottom <= 0 or left >= disp_w or top >= disp_h:
                    continue
                bbox = [int(round(v)) for v in (left, top, right, bottom)]
                if grid_shape == 'circle':
                    draw.ellipse(bbox, outline=outline_color_circle, width=outline_width)
                else:
                    draw.rectangle(bbox, outline=outline_color_square, width=outline_width)
        return pil_img

    def _draw_rect_grid_on_image(self, pil_img, rows=4, cols=6, cell_px=416, offset_x=0, offset_y=0):
        """
        矩形网格绘制也基于原始图像坐标：
        - cell_px 为原始像素单位（默认 416）
        - offset_x/offset_y 为原始像素单位
        """
        from PIL import ImageDraw
        draw = ImageDraw.Draw(pil_img, 'RGBA')
        disp_w, disp_h = pil_img.size
        # compute original grid dims and origin, then map to display coords
        grid_w_orig = cell_px * cols
        grid_h_orig = cell_px * rows
        start_x_orig = (self.orig_w - grid_w_orig) / 2.0 + offset_x
        start_y_orig = (self.orig_h - grid_h_orig) / 2.0 + offset_y

        display_cell = cell_px * self.scale
        display_cell = max(1.0, display_cell)
        grid_w = display_cell * cols
        grid_h = display_cell * rows
        start_x = start_x_orig * self.scale
        start_y = start_y_orig * self.scale

        line_color = (255, 200, 0, 200)
        line_width = max(1, int(round(display_cell * 0.01)))
        for c in range(cols + 1):
            x = int(round(start_x + c * display_cell))
            y0 = int(round(start_y))
            y1 = int(round(start_y + grid_h))
            draw.line([(x, y0), (x, y1)], fill=line_color, width=line_width)
        for r in range(rows + 1):
            y = int(round(start_y + r * display_cell))
            x0 = int(round(start_x))
            x1 = int(round(start_x + grid_w))
            draw.line([(x0, y), (x1, y)], fill=line_color, width=line_width)
        return pil_img

    def compute_circle_stats(self):
        """
        统计网格（圆或方）每格的像素值之和 — 使用原始图像坐标进行计算，结果不会因为窗口缩放/大小变化而改变。
        以表格形式显示默认（带标题）；如需仅每行一条便于复制，请使用“统计网格灰度和（每行一条）”按钮。
        """
        if self.orig_pil is None:
            messagebox.showinfo("统计结果", "尚未加载图像或无显示内容。"); return
        if not self.show_grid_var.get():
            messagebox.showinfo("统计结果", "网格未启用，请先勾选“显示 网格 (圆/方)”。"); return
        stats_upper = None
        try:
            text = self.stats_upper_entry.get().strip()
            if text != "":
                stats_upper = float(text)
        except Exception:
            stats_upper = None

        rows = max(1, int(self.circle_rows_var.get()))
        cols = max(1, int(self.circle_cols_var.get()))
        offset_x = int(self.circle_offset_x)
        offset_y = int(self.circle_offset_y)

        diameter_cm = 0.9
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm
        r_orig = diameter_px_raw / 2.0

        h, w = self.img_arr.shape if self.img_arr.ndim == 2 else self.img_arr.shape[:2]

        grid_w_orig = diameter_px_raw * cols
        grid_h_orig = diameter_px_raw * rows
        start_x_orig = (w - grid_w_orig) / 2.0 + offset_x
        start_y_orig = (h - grid_h_orig) / 2.0 + offset_y

        grid_shape = self.grid_shape_var.get()

        results = []
        for ri in range(rows):
            row_label = index_to_letters(ri)
            for ci in range(cols):
                col_label = str(ci + 1)
                label = f"{row_label}{col_label}"
                cy_orig = start_y_orig + (ri + 0.5) * diameter_px_raw
                cx_orig = start_x_orig + (ci + 0.5) * diameter_px_raw

                y0 = max(0, int(np.floor(cy_orig - r_orig)))
                y1 = min(h, int(np.ceil(cy_orig + r_orig)) + 1)
                x0 = max(0, int(np.floor(cx_orig - r_orig)))
                x1 = min(w, int(np.ceil(cx_orig + r_orig)) + 1)
                if y0 >= y1 or x0 >= x1:
                    results.append((label, 0.0))
                    continue

                yy = np.arange(y0, y1).reshape(-1, 1)
                xx = np.arange(x0, x1).reshape(1, -1)
                if grid_shape == 'circle':
                    mask = (xx - cx_orig) ** 2 + (yy - cy_orig) ** 2 <= (r_orig * r_orig)
                else:
                    # square: full bounding box
                    mask = np.ones((y1 - y0, x1 - x0), dtype=bool)

                sub = self.img_arr[y0:y1, x0:x1]
                if stats_upper is not None:
                    valid = sub <= stats_upper
                    mask = mask & valid

                # sum of grayscale intensities within mask
                sum_values = int(np.sum(sub[mask], dtype=np.uint64))
                results.append((label, sum_values))

        top = tk.Toplevel(self)
        top.title("网格逐格灰度和（按格显示）")
        top.geometry("480x560")
        Label(top, text=f"图片：{self.img_name}", anchor='w').pack(fill=tk.X, padx=8, pady=(8, 2))
        Label(top, text=f"网格：{rows} 行 × {cols} 列（行=A..，列=1..），形状={self.grid_shape_var.get()}", anchor='w').pack(fill=tk.X, padx=8)
        if stats_upper is not None:
            Label(top, text=f"统计忽略阈值：大于 {stats_upper} 的像素不计入总和", anchor='w', fg="#444").pack(fill=tk.X, padx=8)

        frm = Frame(top)
        frm.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        scrollbar = tk.Scrollbar(frm)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        txt = tk.Text(frm, wrap=tk.NONE, yscrollcommand=scrollbar.set, font=("Consolas", 10))
        txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=txt.yview)

        txt.insert(tk.END, "标签\t灰度和\n")
        txt.insert(tk.END, "----\t------\n")
        for label, s in results:
            txt.insert(tk.END, f"{label}\t{s}\n")
        txt.config(state=tk.NORMAL)

        def copy_to_clipboard():
            try:
                data = "标签 灰度和\n" + "\n".join([f"{l} {v}" for l, v in results])
                top.clipboard_clear()
                top.clipboard_append(data)
                messagebox.showinfo("复制成功", "已将结果（含标题）复制到剪贴板。")
            except Exception as e:
                messagebox.showerror("复制失败", f"错误：{e}")

        Button(top, text="复制结果到剪贴板", command=copy_to_clipboard).pack(pady=(0, 8))

    def compute_grid_pixel_sum_lines(self):
        """
        将每个网格（圆或方）内的灰度和按一行一条的形式显示，便于复制：
        输出示例：
        A1 100
        A2 500
        A3 200
        """
        if self.orig_pil is None:
            messagebox.showinfo("统计结果", "尚未加载图像或无显示内容。"); return
        if not self.show_grid_var.get():
            messagebox.showinfo("统计结果", "网格未启用，请先勾选“显示 网格 (圆/方)”。"); return
        stats_upper = None
        try:
            text = self.stats_upper_entry.get().strip()
            if text != "":
                stats_upper = float(text)
        except Exception:
            stats_upper = None

        rows = max(1, int(self.circle_rows_var.get()))
        cols = max(1, int(self.circle_cols_var.get()))
        offset_x = int(self.circle_offset_x)
        offset_y = int(self.circle_offset_y)

        diameter_cm = 0.9
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm
        r_orig = diameter_px_raw / 2.0

        h, w = self.img_arr.shape if self.img_arr.ndim == 2 else self.img_arr.shape[:2]

        grid_w_orig = diameter_px_raw * cols
        grid_h_orig = diameter_px_raw * rows
        start_x_orig = (w - grid_w_orig) / 2.0 + offset_x
        start_y_orig = (h - grid_h_orig) / 2.0 + offset_y

        grid_shape = self.grid_shape_var.get()

        lines = []
        for ri in range(rows):
            for ci in range(cols):
                label = f"{index_to_letters(ri)}{ci+1}"
                cy_orig = start_y_orig + (ri + 0.5) * diameter_px_raw
                cx_orig = start_x_orig + (ci + 0.5) * diameter_px_raw

                y0 = max(0, int(np.floor(cy_orig - r_orig)))
                y1 = min(h, int(np.ceil(cy_orig + r_orig)) + 1)
                x0 = max(0, int(np.floor(cx_orig - r_orig)))
                x1 = min(w, int(np.ceil(cx_orig + r_orig)) + 1)
                if y0 >= y1 or x0 >= x1:
                    lines.append(f"{label} 0")
                    continue

                yy = np.arange(y0, y1).reshape(-1, 1)
                xx = np.arange(x0, x1).reshape(1, -1)
                if grid_shape == 'circle':
                    mask = (xx - cx_orig) ** 2 + (yy - cy_orig) ** 2 <= (r_orig * r_orig)
                else:
                    mask = np.ones((y1 - y0, x1 - x0), dtype=bool)

                sub = self.img_arr[y0:y1, x0:x1]
                if stats_upper is not None:
                    valid = sub <= stats_upper
                    mask = mask & valid

                sum_values = int(np.sum(sub[mask], dtype=np.uint64))
                lines.append(f"{label} {sum_values}")

        top = tk.Toplevel(self)
        top.title("网格灰度和（每行一条，便于复制）")
        top.geometry("420x560")
        Label(top, text=f"图片：{self.img_name}", anchor='w').pack(fill=tk.X, padx=8, pady=(8, 2))
        Label(top, text=f"网格：{rows} 行 × {cols} 列，形状={self.grid_shape_var.get()}", anchor='w').pack(fill=tk.X, padx=8)
        if stats_upper is not None:
            Label(top, text=f"统计忽略阈值：大于 {stats_upper} 的像素不计入总和", anchor='w', fg="#444").pack(fill=tk.X, padx=8)

        frm = Frame(top)
        frm.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        scrollbar = tk.Scrollbar(frm)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        txt = tk.Text(frm, wrap=tk.NONE, yscrollcommand=scrollbar.set, font=("Consolas", 12))
        txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=txt.yview)

        txt.insert(tk.END, "\n".join(lines))
        txt.config(state=tk.NORMAL)

        def copy_lines():
            try:
                data = "\n".join(lines)
                top.clipboard_clear()
                top.clipboard_append(data)
                messagebox.showinfo("复制成功", "已将每行一条的结果复制到剪贴板。")
            except Exception as e:
                messagebox.showerror("复制失败", f"错误：{e}")

        Button(top, text="复制每行一条到剪贴板", command=copy_lines).pack(pady=(0, 8))

    # -------------------------
    # run_detection (v2：按网格内检测 + watershed 拆分)
    # -------------------------
    def run_detection(self):
        try:
            use_otsu = bool(self.use_otsu_var.get())
            min_pixels = int(self.min_pixels_entry.get())
            max_pixels = int(self.max_pixels_entry.get())
        except Exception as e:
            messagebox.showerror("参数错误", f"请检查检测参数输入：{e}")
            return

        rows = self.circle_rows_var.get()
        cols = self.circle_cols_var.get()
        offset_x = self.circle_offset_x
        offset_y = self.circle_offset_y
        diameter_cm = 0.9
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm

        h, w = self.img_arr.shape if self.img_arr.ndim == 2 else self.img_arr.shape[:2]
        r_orig = diameter_px_raw / 2.0

        grid_shape = self.grid_shape_var.get()

        all_detected_regions = []
        self.circle_regions = []

        for ri in range(rows):
            for ci in range(cols):
                label = f"{index_to_letters(ri)}{ci+1}"

                grid_w_orig = diameter_px_raw * cols
                grid_h_orig = diameter_px_raw * rows
                start_x_orig = (w - grid_w_orig) / 2.0 + offset_x
                start_y_orig = (h - grid_h_orig) / 2.0 + offset_y
                cx_orig = start_x_orig + (ci + 0.5) * diameter_px_raw
                cy_orig = start_y_orig + (ri + 0.5) * diameter_px_raw

                y0 = max(0, int(np.floor(cy_orig - r_orig)))
                y1 = min(h, int(np.ceil(cy_orig + r_orig)) + 1)
                x0 = max(0, int(np.floor(cx_orig - r_orig)))
                x1 = min(w, int(np.ceil(cx_orig + r_orig)) + 1)
                if y0 >= y1 or x0 >= x1:
                    self.circle_regions.append((label, []))
                    continue

                sub_img_arr = self.img_arr[y0:y1, x0:x1]
                if sub_img_arr.size == 0:
                    self.circle_regions.append((label, []))
                    continue

                yy, xx = np.ogrid[y0:y1, x0:x1]
                if grid_shape == 'circle':
                    circle_mask = (xx - cx_orig) ** 2 + (yy - cy_orig) ** 2 <= r_orig ** 2
                else:
                    circle_mask = np.ones((y1 - y0, x1 - x0), dtype=bool)  # square bounding box

                if not circle_mask.any():
                    self.circle_regions.append((label, []))
                    continue

                img = sub_img_arr.copy()
                img_float = util.img_as_float(img)

                if use_otsu:
                    try:
                        th = filters.threshold_otsu(img_float)
                        mask = img_float > th
                    except Exception:
                        mask = img_float > 0
                else:
                    mask = img_float > 0

                mask &= circle_mask

                selem = morphology.disk(1)
                mask = morphology.binary_closing(mask, selem)
                mask = morphology.binary_opening(mask, selem)

                mask = mask.astype(bool)
                if not mask.any():
                    self.circle_regions.append((label, []))
                    continue

                labels = measure.label(mask, connectivity=2)
                props = measure.regionprops(labels)
                accepted_regions = []

                def split_region_recursive(prop_label_mask, bbox, img_patch, min_pixels_local, max_pixels_local):
                    this_pixels = int(np.sum(prop_label_mask))
                    if this_pixels < min_pixels_local:
                        return []
                    if max_pixels_local <= 0 or this_pixels <= max_pixels_local:
                        coords = np.column_stack(np.nonzero(prop_label_mask))
                        coords_global = np.column_stack((coords[:, 0] + bbox[0] + y0, coords[:, 1] + bbox[1] + x0))
                        rr = coords_global[:, 0]
                        cc = coords_global[:, 1]
                        cy = float(np.mean(rr))
                        cx = float(np.mean(cc))
                        return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                    submask = prop_label_mask.copy()
                    if submask.size == 0 or submask.shape[0] == 0 or submask.shape[1] == 0:
                        return []
                    sub_img = img_patch.copy()
                    if sub_img.size == 0 or sub_img.shape[0] == 0 or sub_img.shape[1] == 0:
                        return []
                    sub_smooth = filters.gaussian(sub_img, sigma=1.0)

                    try:
                        local_maxi = feature.peak_local_max(sub_smooth, return_indices=False, min_distance=1, labels=submask)
                        coords_pk = np.column_stack(np.nonzero(local_maxi))
                    except Exception:
                        try:
                            local_max = (sub_smooth == ndi.maximum_filter(sub_smooth, size=(3, 3))) & submask
                            coords_pk = np.column_stack(np.nonzero(local_max))
                        except Exception:
                            coords_pk = np.array([]).reshape(0, 2)

                    peak_coords = []
                    if len(coords_pk) > 0:
                        intensities = sub_smooth[coords_pk[:, 0], coords_pk[:, 1]]
                        try:
                            p75 = np.percentile(sub_smooth[submask], 75)
                        except Exception:
                            p75 = np.max(intensities) if intensities.size else 0
                        strong_idx = np.where(intensities >= p75)[0]
                        if len(strong_idx) >= 2:
                            peak_coords = coords_pk[strong_idx]
                        else:
                            try:
                                p50 = np.percentile(sub_smooth[submask], 50) if np.any(submask) else np.max(intensities)
                            except Exception:
                                p50 = np.max(intensities) if intensities.size else 0
                            strong_idx2 = np.where(intensities >= p50)[0]
                            if len(strong_idx2) >= 2:
                                peak_coords = coords_pk[strong_idx2]
                            else:
                                peak_coords = coords_pk

                    peak_distance = None
                    if len(peak_coords) >= 2:
                        pts = peak_coords[:, ::-1].astype(float)
                        try:
                            tree = cKDTree(pts)
                            dists, _ = tree.query(pts, k=2)
                            nn = dists[:, 1]
                            median_nn = float(np.median(nn))
                            peak_distance = max(2, int(max(1, median_nn * 0.8)))
                        except Exception:
                            peak_distance = None

                    if peak_distance is None:
                        h_sub, w_sub = submask.shape
                        peak_distance = max(4, int(min(h_sub, w_sub) / 8))
                    peak_distance = max(2, min(peak_distance, max(4, min(submask.shape) // 2)))

                    distance = ndi.distance_transform_edt(submask)
                    try:
                        local_maxi2 = feature.peak_local_max(distance, return_indices=False, min_distance=peak_distance, labels=submask)
                    except Exception:
                        try:
                            footprint = morphology.disk(max(1, peak_distance))
                            local_maxi2 = (distance == ndi.maximum_filter(distance, footprint=footprint)) & submask
                        except Exception:
                            local_maxi2 = np.zeros_like(distance, dtype=bool)

                    markers, _ = ndi.label(local_maxi2)
                    if markers.max() == 0:
                        coords2 = np.column_stack(np.nonzero(submask))
                        coords_global = np.column_stack((coords2[:, 0] + bbox[0] + y0, coords2[:, 1] + bbox[1] + x0))
                        rr = coords_global[:, 0]
                        cc = coords_global[:, 1]
                        cy = float(np.mean(rr))
                        cx = float(np.mean(cc))
                        return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                    sub_labels = segmentation.watershed(-distance, markers, mask=submask)
                    if min_pixels_local > 0:
                        sub_labels = morphology.remove_small_objects(sub_labels, min_size=min_pixels_local)

                    sub_props = measure.regionprops(sub_labels)
                    if len(sub_props) <= 1:
                        coords2 = np.column_stack(np.nonzero(submask))
                        coords_global = np.column_stack((coords2[:, 0] + bbox[0] + y0, coords2[:, 1] + bbox[1] + x0))
                        rr = coords_global[:, 0]
                        cc = coords_global[:, 1]
                        cy = float(np.mean(rr))
                        cx = float(np.mean(cc))
                        return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                    results = []
                    for sp in sub_props:
                        sp_minr, sp_minc, sp_maxr, sp_maxc = sp.bbox
                        global_minr = bbox[0] + sp_minr
                        global_minc = bbox[1] + sp_minc
                        global_maxr = bbox[0] + sp_maxr
                        global_maxc = bbox[1] + sp_maxc
                        img_patch_sp = img_patch[global_minr:global_maxr, global_minc:global_maxc]
                        sp_mask_in_patch = (sub_labels[sp_minr:sp_maxr, sp_minc:sp_maxc] == sp.label)
                        results.extend(
                            split_region_recursive(
                                sp_mask_in_patch,
                                (global_minr, global_minc, global_maxr, global_maxc),
                                img_patch_sp,
                                min_pixels_local,
                                max_pixels_local
                            )
                        )
                    return results

                for prop in props:
                    pixel_count = int(prop.area)
                    if pixel_count < min_pixels:
                        continue
                    if max_pixels <= 0 or pixel_count <= max_pixels:
                        coords = prop.coords
                        coords_global = np.column_stack((coords[:, 0] + y0, coords[:, 1] + x0))
                        rr = coords_global[:, 0]
                        cc = coords_global[:, 1]
                        cy = float(np.mean(rr))
                        cx = float(np.mean(cc))
                        accepted_regions.append(SimpleRegion(pixel_count, (cy, cx), coords_global))
                    else:
                        minr, minc, maxr, maxc = prop.bbox
                        prop_mask = (labels == prop.label)[minr:maxr, minc:maxc]
                        img_patch = img_float[minr:maxr, minc:maxc]
                        parts = split_region_recursive(prop_mask, (minr, minc, maxr, maxc), img_patch, min_pixels, max_pixels)
                        accepted_regions.extend(parts)

                all_detected_regions.extend(accepted_regions)
                self.circle_regions.append((label, accepted_regions))

        self.detected_regions = all_detected_regions

        if self.show_big_spots_var.get() and self.detected_regions:
            try:
                max_pixels_val = int(self.max_pixels_entry.get())
            except Exception:
                max_pixels_val = 0
            self.annotated_overlay = self._build_colored_overlay(self.detected_regions, max_pixels=max_pixels_val)
        else:
            self.annotated_overlay = None

        self.update_image_display()
        self._show_detection_results()

    def save_annotated_image(self):
        if self.annotated_overlay is None and not self.detected_regions:
            messagebox.showinfo("保存失败", "尚未进行检测或未生成标注图。")
            return
        base = self.orig_pil.convert('RGBA')
        overlay_to_save = self.annotated_overlay
        if overlay_to_save is None and self.detected_regions:
            try:
                max_pixels_val = int(self.max_pixels_entry.get())
            except Exception:
                max_pixels_val = 0
            overlay_to_save = self._build_colored_overlay(self.detected_regions, max_pixels=max_pixels_val)
        composed = Image.alpha_composite(base, overlay_to_save) if overlay_to_save is not None else base

        path = filedialog.asksaveasfilename(
            defaultextension=".png",
            filetypes=[("PNG image", "*.png"), ("JPEG", "*.jpg"), ("TIFF", "*.tif;*.tiff")]
        )
        if not path:
            return
        try:
            composed.save(path)
            messagebox.showinfo("保存成功", f"标注图已保存到：\n{path}")
        except Exception as e:
            messagebox.showerror("保存失败", f"无法保存文件：{e}")

    def _show_detection_results(self):
        top = tk.Toplevel(self)
        top.title("检测结果：每个网格内大亮点总个数")
        top.geometry("600x600")
        Label(top, text=f"图片：{self.img_name}", anchor='w').pack(fill=tk.X, padx=8, pady=(8, 2))
        Label(top, text=f"总检测到目标数量：{len(self.detected_regions)}", anchor='w').pack(fill=tk.X, padx=8)

        frm = Frame(top)
        frm.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        scrollbar = tk.Scrollbar(frm)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        txt = tk.Text(frm, wrap=tk.NONE, yscrollcommand=scrollbar.set, font=("Consolas", 10))
        txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=txt.yview)

        txt.insert(tk.END, "网格\t亮点总个数\n")
        txt.insert(tk.END, "------\t------------\n")
        for label, regions in self.circle_regions:
            txt.insert(tk.END, f"{label}\t{len(regions)}\n")
        txt.config(state=tk.NORMAL)

        def copy_cb():
            try:
                lines = ["网格\t亮点总个数"]
                for label, regions in self.circle_regions:
                    lines.append(f"{label}\t{len(regions)}")
                top.clipboard_clear()
                top.clipboard_append("\n".join(lines))
                messagebox.showinfo("复制成功", "已将结果复制到剪贴板。")
            except Exception as e:
                messagebox.showerror("复制失败", f"错误：{e}")

        Button(top, text="复制结果到剪贴板", command=copy_cb).pack(pady=(0, 8))

    def show_pixel_info(self, event):
        if self.img_arr is None or self.scale == 0:
            return
        canvas_x, canvas_y = event.x, event.y
        img_x = int((canvas_x - self.image_pos[0]) / max(self.scale, 1e-9))
        img_y = int((canvas_y - self.image_pos[1]) / max(self.scale, 1e-9))
        h, w = self.img_arr.shape if len(self.img_arr.shape) == 2 else self.img_arr.shape[:2]
        if 0 <= img_x < w and 0 <= img_y < h:
            value = int(self.img_arr[img_y, img_x])
            if not hasattr(self, 'tooltip') or self.tooltip is None:
                self.tooltip = Label(self.canvas, bg="#ffffe0", fg="black", bd=1, relief=tk.SOLID, font=("Arial", 10))
            tip_x = min(event.x + 20, self.canvas.winfo_width() - 80)
            tip_y = min(event.y + 10, self.canvas.winfo_height() - 20)
            self.tooltip.config(text=f"({img_x},{img_y}) : {value}")
            self.tooltip.place(x=tip_x, y=tip_y)
        else:
            if hasattr(self, 'tooltip') and self.tooltip is not None:
                self.tooltip.place_forget()

    def on_resize(self, event):
        if self.user_zoom:
            self.redraw_image()
        else:
            self.update_image_display()

    def close_panel(self):
        self.remove_callback(self)
        self.destroy()

    # -------------------------
    # 新增：保存网格统计为 TIF（在图上绘制网格及每格编号与统计值）
    # -------------------------
    def save_grid_stats_as_tif(self):
        """
        计算当前设置下每个网格内的灰度和（可使用 stats_upper 忽略高值），生成一张带网格与每格文字的 RGB TIFF，并保存。
        文本格式示例：A1：1000
        """
        if self.orig_pil is None:
            messagebox.showinfo("保存失败", "尚未加载图像或无显示内容。"); return
        if not self.show_grid_var.get():
            messagebox.showinfo("保存失败", "网格未启用，请先勾选“显示 网格 (圆/方)”。"); return

        try:
            stats_upper = None
            text = self.stats_upper_entry.get().strip()
            if text != "":
                stats_upper = float(text)
        except Exception:
            stats_upper = None

        rows = max(1, int(self.circle_rows_var.get()))
        cols = max(1, int(self.circle_cols_var.get()))
        offset_x = int(self.circle_offset_x)
        offset_y = int(self.circle_offset_y)

        diameter_cm = 0.9
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm
        r_orig = diameter_px_raw / 2.0

        h, w = self.img_arr.shape if self.img_arr.ndim == 2 else self.img_arr.shape[:2]

        grid_w_orig = diameter_px_raw * cols
        grid_h_orig = diameter_px_raw * rows
        start_x_orig = (w - grid_w_orig) / 2.0 + offset_x
        start_y_orig = (h - grid_h_orig) / 2.0 + offset_y

        grid_shape = self.grid_shape_var.get()

        # 计算每格灰度和
        sum_counts = {}
        for ri in range(rows):
            for ci in range(cols):
                label = f"{index_to_letters(ri)}{ci+1}"
                cy_orig = start_y_orig + (ri + 0.5) * diameter_px_raw
                cx_orig = start_x_orig + (ci + 0.5) * diameter_px_raw

                y0 = max(0, int(np.floor(cy_orig - r_orig)))
                y1 = min(h, int(np.ceil(cy_orig + r_orig)) + 1)
                x0 = max(0, int(np.floor(cx_orig - r_orig)))
                x1 = min(w, int(np.ceil(cx_orig + r_orig)) + 1)
                if y0 >= y1 or x0 >= x1:
                    sum_counts[label] = 0
                    continue

                yy = np.arange(y0, y1).reshape(-1, 1)
                xx = np.arange(x0, x1).reshape(1, -1)
                if grid_shape == 'circle':
                    mask = (xx - cx_orig) ** 2 + (yy - cy_orig) ** 2 <= (r_orig * r_orig)
                else:
                    mask = np.ones((y1 - y0, x1 - x0), dtype=bool)

                sub = self.img_arr[y0:y1, x0:x1]
                if stats_upper is not None:
                    valid = sub <= stats_upper
                    mask = mask & valid

                sum_values = int(np.sum(sub[mask], dtype=np.uint64))
                sum_counts[label] = sum_values

        # circle_params used by draw_grid_and_text_on_sum_image
        circle_params = {
            "rows": rows,
            "cols": cols,
            "offset_x": offset_x,
            "offset_y": offset_y,
            "diameter_cm": diameter_cm,
            "diameter_px_raw": diameter_px_raw,
            "grid_shape": grid_shape,
        }

        # 使用 draw_grid_and_text_on_sum_image 绘制标签与网格（以原始 img_arr 作为背景）
        try:
            base_arr = self.img_arr.astype(np.uint16)
            pil_out = draw_grid_and_text_on_sum_image(base_arr, circle_params, sum_counts)
        except Exception as e:
            messagebox.showerror("生成失败", f"生成带网格标签图像失败：{e}")
            return

        # 建议默认文件名
        default_name = os.path.splitext(self.img_name)[0] + "_grid_sum.tif"
        path = filedialog.asksaveasfilename(
            defaultextension=".tif",
            initialfile=default_name,
            filetypes=[("TIFF image", "*.tif;*.tiff"), ("PNG image", "*.png")]
        )
        if not path:
            return
        try:
            # 将 PIL 图像保存为 tif
            # 如果需要保存为多页或保持元数据，可使用 tifffile.imwrite(np.array(pil_out))
            pil_out.save(path)
            messagebox.showinfo("保存成功", f"网格统计图已保存到：\n{path}")
        except Exception as e:
            messagebox.showerror("保存失败", f"无法保存文件：{e}")


# ============================================================
# Progress dialog (批量进度)
# ============================================================
class ProgressDialog(tk.Toplevel):
    def __init__(self, master, title="批量处理进度"):
        super().__init__(master)
        self.title(title)
        self.geometry("520x160")
        self.resizable(False, False)
        self.protocol("WM_DELETE_WINDOW", lambda: None)

        self.label_main = Label(self, text="准备开始...", anchor="w")
        self.label_main.pack(fill=tk.X, padx=10, pady=(10, 4))
        self.label_detail = Label(self, text="", anchor="w", fg="#444")
        self.label_detail.pack(fill=tk.X, padx=10, pady=(0, 8))

        self.canvas = tk.Canvas(self, height=18, bg="#eee", highlightthickness=0)
        self.canvas.pack(fill=tk.X, padx=10, pady=(0, 8))
        self._bar = self.canvas.create_rectangle(0, 0, 0, 18, fill="#1e90ff", width=0)

        self._total = 1

    def set_total(self, total: int):
        self._total = max(1, int(total))
        self.set_progress(0, "")

    def set_progress(self, current_index: int, detail: str):
        cur = max(0, int(current_index))
        self.label_main.config(text=f"处理中：{cur}/{self._total}")
        self.label_detail.config(text=detail)
        w = max(1, self.canvas.winfo_width())
        frac = min(1.0, float(cur) / float(self._total))
        self.canvas.coords(self._bar, 0, 0, int(w * frac), 18)
        self.update_idletasks()


# ============================================================
# Main app (v2 + 批量检测)
# ============================================================
class MultiTifViewer(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("多图TIF浏览器（相近大亮点不同颜色标注）")
        self.geometry("1200x800")
        self.panel_container = tk.Frame(self)
        self.panel_container.pack(fill=tk.BOTH, expand=True)

        self.menu_bar = tk.Menu(self)
        self.menu_bar.add_command(label="添加图片", command=self.open_image)
        self.menu_bar.add_command(label="批量检测同文件夹（使用当前选中图参数）", command=self.batch_detect_folder)
        self.config(menu=self.menu_bar)

        self.panels = []
        self.selected_panel = None

    def set_selected_panel(self, panel: ImagePanel):
        self.selected_panel = panel
        for p in self.panels:
            p.set_selected(p is panel)

    def open_image(self):
        filepaths = filedialog.askopenfilenames(filetypes=[("TIFF files", "*.tif;*.tiff"), ("All files", "*.*")])
        for file_path in filepaths:
            try:
                img = Image.open(file_path)
                if img.mode not in ('L', 'I;16'):
                    try:
                        img = img.convert('I;16')
                    except Exception:
                        img = img.convert('L')
                arr = np.array(img)
            except Exception as e:
                messagebox.showerror("打开失败", f"无法打开文件：\n{file_path}\n\n错误：{e}")
                continue

            name = os.path.basename(file_path)
            panel = ImagePanel(
                self.panel_container,
                arr,
                name,
                self.remove_panel,
                img_path=file_path,
                select_callback=self.set_selected_panel
            )
            panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=5, pady=5)
            self.panels.append(panel)
            self.set_selected_panel(panel)

    def remove_panel(self, panel):
        if panel in self.panels:
            self.panels.remove(panel)
            panel.destroy()
            if self.selected_panel is panel:
                self.selected_panel = self.panels[0] if self.panels else None
                if self.selected_panel is not None:
                    self.set_selected_panel(self.selected_panel)

    def _list_tif_files(self, folder: str):
        if not folder or not os.path.isdir(folder):
            return []
        files = []
        for n in os.listdir(folder):
            low = n.lower()
            if low.endswith(".tif") or low.endswith(".tiff"):
                files.append(os.path.join(folder, n))
        files.sort()
        return files

    def batch_detect_folder(self):
        """
        批量检测（完整实现）：
        - .txt/.tif 同名直接覆盖
        - 叠加像素饱和到 65535
        - 支持网格形状：circle / square（由当前选中图片的参数决定）
        """
        if self.selected_panel is None:
            messagebox.showinfo("批量检测", "请先打开至少一张图片，并单击选中一张图片作为参数来源。")
            return

        src = self.selected_panel
        if not src.img_path or not os.path.isfile(src.img_path):
            messagebox.showerror("批量检测", "当前选中图片没有有效文件路径，无法定位文件夹。")
            return

        folder = os.path.dirname(src.img_path)
        tif_files = self._list_tif_files(folder)
        if not tif_files:
            messagebox.showinfo("批量检测", f"文件夹中未找到 tif/tiff：\n{folder}")
            return

        try:
            use_otsu = bool(src.use_otsu_var.get())
            min_pixels = int(src.min_pixels_entry.get())
            max_pixels = int(src.max_pixels_entry.get())
        except Exception as e:
            messagebox.showerror("参数错误", f"请检查检测参数输入：{e}")
            return
        detect_params = {"use_otsu": use_otsu, "min_pixels": min_pixels, "max_pixels": max_pixels}

        try:
            rows = int(src.circle_rows_var.get())
            cols = int(src.circle_cols_var.get())
            offset_x = int(src.circle_offset_x)
            offset_y = int(src.circle_offset_y)
            grid_shape = src.grid_shape_var.get()
        except Exception as e:
            messagebox.showerror("参数错误", f"请检查网格参数输入：{e}")
            return

        diameter_cm = 0.9
        px_per_cm = 416.0 / 2.0
        diameter_px_raw = diameter_cm * px_per_cm

        circle_params = {
            "rows": rows,
            "cols": cols,
            "offset_x": offset_x,
            "offset_y": offset_y,
            "diameter_cm": diameter_cm,
            "diameter_px_raw": diameter_px_raw,
            "grid_shape": grid_shape,
        }

        label_order = circle_label_order(rows, cols)
        sum_counts = {lab: 0 for lab in label_order}

        total_steps = len(tif_files) + 2
        prog = ProgressDialog(self, title="批量检测进度（同文件夹）")
        prog.set_total(total_steps)
        prog.set_progress(0, f"文件夹：{folder}")

        ok = 0
        fail = 0
        errors = []
        summary_txt_path = ""
        raw16_path = ""
        vis_path = ""

        # ============================================================
        # 逐图检测（per-grid 检测）
        # ============================================================
        for i, path in enumerate(tif_files, start=1):
            prog.set_progress(i, f"检测：{os.path.basename(path)}")
            try:
                img_arr = read_tif_as_array(path)
                if img_arr.ndim != 2:
                    raise ValueError(f"只支持2D灰度图：{os.path.basename(path)} shape={img_arr.shape}")

                h, w = img_arr.shape
                r_orig = diameter_px_raw / 2.0

                grid_w_orig = diameter_px_raw * cols
                grid_h_orig = diameter_px_raw * rows
                start_x_orig = (w - grid_w_orig) / 2.0 + offset_x
                start_y_orig = (h - grid_h_orig) / 2.0 + offset_y

                circle_regions = []
                all_regions = []

                for ri in range(rows):
                    for ci in range(cols):
                        lab = f"{index_to_letters(ri)}{ci+1}"
                        cx_orig = start_x_orig + (ci + 0.5) * diameter_px_raw
                        cy_orig = start_y_orig + (ri + 0.5) * diameter_px_raw

                        y0 = max(0, int(np.floor(cy_orig - r_orig)))
                        y1 = min(h, int(np.ceil(cy_orig + r_orig)) + 1)
                        x0 = max(0, int(np.floor(cx_orig - r_orig)))
                        x1 = min(w, int(np.ceil(cx_orig + r_orig)) + 1)
                        if y0 >= y1 or x0 >= x1:
                            circle_regions.append((lab, []))
                            continue

                        sub_img_arr = img_arr[y0:y1, x0:x1]
                        if sub_img_arr.size == 0:
                            circle_regions.append((lab, []))
                            continue

                        yy, xx = np.ogrid[y0:y1, x0:x1]
                        if grid_shape == 'circle':
                            circle_mask = (xx - cx_orig) ** 2 + (yy - cy_orig) ** 2 <= r_orig ** 2
                        else:
                            circle_mask = np.ones((y1 - y0, x1 - x0), dtype=bool)

                        if not circle_mask.any():
                            circle_regions.append((lab, []))
                            continue

                        img_float = util.img_as_float(sub_img_arr)

                        if use_otsu:
                            try:
                                th = filters.threshold_otsu(img_float)
                                mask = img_float > th
                            except Exception:
                                mask = img_float > 0
                        else:
                            mask = img_float > 0

                        mask &= circle_mask

                        selem = morphology.disk(1)
                        mask = morphology.binary_closing(mask, selem)
                        mask = morphology.binary_opening(mask, selem)

                        mask = mask.astype(bool)
                        if not mask.any():
                            circle_regions.append((lab, []))
                            continue

                        labels_cc = measure.label(mask, connectivity=2)
                        props = measure.regionprops(labels_cc)

                        accepted = []

                        def split_region_recursive(prop_label_mask, bbox, img_patch, min_pixels_local, max_pixels_local):
                            this_pixels = int(np.sum(prop_label_mask))
                            if this_pixels < min_pixels_local:
                                return []
                            if max_pixels_local <= 0 or this_pixels <= max_pixels_local:
                                coords = np.column_stack(np.nonzero(prop_label_mask))
                                coords_global = np.column_stack((coords[:, 0] + bbox[0] + y0, coords[:, 1] + bbox[1] + x0))
                                rr = coords_global[:, 0]
                                cc = coords_global[:, 1]
                                cy = float(np.mean(rr))
                                cx = float(np.mean(cc))
                                return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                            submask = prop_label_mask.copy()
                            if submask.size == 0 or submask.shape[0] == 0 or submask.shape[1] == 0:
                                return []
                            sub_img = img_patch.copy()
                            if sub_img.size == 0 or sub_img.shape[0] == 0 or sub_img.shape[1] == 0:
                                return []
                            sub_smooth = filters.gaussian(sub_img, sigma=1.0)

                            try:
                                local_maxi = feature.peak_local_max(sub_smooth, return_indices=False, min_distance=1, labels=submask)
                                coords_pk = np.column_stack(np.nonzero(local_maxi))
                            except Exception:
                                try:
                                    local_max = (sub_smooth == ndi.maximum_filter(sub_smooth, size=(3, 3))) & submask
                                    coords_pk = np.column_stack(np.nonzero(local_max))
                                except Exception:
                                    coords_pk = np.array([]).reshape(0, 2)

                            peak_coords = []
                            if len(coords_pk) > 0:
                                intensities = sub_smooth[coords_pk[:, 0], coords_pk[:, 1]]
                                try:
                                    p75 = np.percentile(sub_smooth[submask], 75)
                                except Exception:
                                    p75 = np.max(intensities) if intensities.size else 0
                                strong_idx = np.where(intensities >= p75)[0]
                                if len(strong_idx) >= 2:
                                    peak_coords = coords_pk[strong_idx]
                                else:
                                    try:
                                        p50 = np.percentile(sub_smooth[submask], 50) if np.any(submask) else np.max(intensities)
                                    except Exception:
                                        p50 = np.max(intensities) if intensities.size else 0
                                    strong_idx2 = np.where(intensities >= p50)[0]
                                    if len(strong_idx2) >= 2:
                                        peak_coords = coords_pk[strong_idx2]
                                    else:
                                        peak_coords = coords_pk

                            peak_distance = None
                            if len(peak_coords) >= 2:
                                pts = peak_coords[:, ::-1].astype(float)
                                try:
                                    tree = cKDTree(pts)
                                    dists, _ = tree.query(pts, k=2)
                                    nn = dists[:, 1]
                                    median_nn = float(np.median(nn))
                                    peak_distance = max(2, int(max(1, median_nn * 0.8)))
                                except Exception:
                                    peak_distance = None

                            if peak_distance is None:
                                h_sub, w_sub = submask.shape
                                peak_distance = max(4, int(min(h_sub, w_sub) / 8))
                            peak_distance = max(2, min(peak_distance, max(4, min(submask.shape) // 2)))

                            distance = ndi.distance_transform_edt(submask)
                            try:
                                local_maxi2 = feature.peak_local_max(distance, return_indices=False, min_distance=peak_distance, labels=submask)
                            except Exception:
                                try:
                                    footprint = morphology.disk(max(1, peak_distance))
                                    local_maxi2 = (distance == ndi.maximum_filter(distance, footprint=footprint)) & submask
                                except Exception:
                                    local_maxi2 = np.zeros_like(distance, dtype=bool)

                            markers, _ = ndi.label(local_maxi2)
                            if markers.max() == 0:
                                coords2 = np.column_stack(np.nonzero(submask))
                                coords_global = np.column_stack((coords2[:, 0] + bbox[0] + y0, coords2[:, 1] + bbox[1] + x0))
                                rr = coords_global[:, 0]
                                cc = coords_global[:, 1]
                                cy = float(np.mean(rr))
                                cx = float(np.mean(cc))
                                return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                            sub_labels = segmentation.watershed(-distance, markers, mask=submask)
                            if min_pixels_local > 0:
                                sub_labels = morphology.remove_small_objects(sub_labels, min_size=min_pixels_local)

                            sub_props = measure.regionprops(sub_labels)
                            if len(sub_props) <= 1:
                                coords2 = np.column_stack(np.nonzero(submask))
                                coords_global = np.column_stack((coords2[:, 0] + bbox[0] + y0, coords2[:, 1] + bbox[1] + x0))
                                rr = coords_global[:, 0]
                                cc = coords_global[:, 1]
                                cy = float(np.mean(rr))
                                cx = float(np.mean(cc))
                                return [SimpleRegion(pixel_count=this_pixels, centroid=(cy, cx), coords=coords_global)]

                            results = []
                            for sp in sub_props:
                                sp_minr, sp_minc, sp_maxr, sp_maxc = sp.bbox
                                global_minr = bbox[0] + sp_minr
                                global_minc = bbox[1] + sp_minc
                                global_maxr = bbox[0] + sp_maxr
                                global_maxc = bbox[1] + sp_maxc
                                img_patch_sp = img_patch[global_minr:global_maxr, global_minc:global_maxc]
                                sp_mask_in_patch = (sub_labels[sp_minr:sp_maxr, sp_minc:sp_maxc] == sp.label)
                                results.extend(
                                    split_region_recursive(
                                        sp_mask_in_patch,
                                        (global_minr, global_minc, global_maxr, global_maxc),
                                        img_patch_sp,
                                        min_pixels_local,
                                        max_pixels_local
                                    )
                                )
                            return results

                        for prop in props:
                            pc = int(prop.area)
                            if pc < min_pixels:
                                continue
                            if max_pixels <= 0 or pc <= max_pixels:
                                coords = prop.coords
                                coords_global = np.column_stack((coords[:, 0] + y0, coords[:, 1] + x0))
                                rr = coords_global[:, 0]
                                cc = coords_global[:, 1]
                                cy = float(np.mean(rr))
                                cx = float(np.mean(cc))
                                accepted.append(SimpleRegion(pc, (cy, cx), coords_global))
                            else:
                                minr, minc, maxr, maxc = prop.bbox
                                prop_mask = (labels_cc == prop.label)[minr:maxr, minc:maxc]
                                img_patch = img_float[minr:maxr, minc:maxc]
                                accepted.extend(split_region_recursive(prop_mask, (minr, minc, maxr, maxc), img_patch, min_pixels, max_pixels))

                        circle_regions.append((lab, accepted))
                        all_regions.extend(accepted)

                # 单图 txt（覆盖同名）
                save_per_image_counts_txt(
                    img_path=path,
                    circle_regions=circle_regions,
                    total_count=len(all_regions),
                    detect_params=detect_params,
                    circle_params=circle_params
                )

                for lab, regs in circle_regions:
                    sum_counts[lab] = int(sum_counts.get(lab, 0)) + len(regs)

                ok += 1

            except Exception as e:
                fail += 1
                errors.append(f"{os.path.basename(path)}: {e}")

            self.update_idletasks()

        # 汇总 txt（覆盖同名）
        try:
            summary_txt_path = save_folder_summary_txt(folder, tif_files, label_order, sum_counts, detect_params, circle_params)
        except Exception as e:
            fail += 1
            errors.append(f"写汇总TXT失败: {e}")

        # 叠加 tif（饱和到 65535）+ 可视化 tif（覆盖同名）
        try:
            prog.set_progress(len(tif_files) + 1, "生成逐像素叠加图（饱和到65535）...")
            sum16 = build_folder_pixel_sum_saturated_uint16(tif_files)

            prog.set_progress(len(tif_files) + 1, "生成可视化叠加图（网格+文字）...")
            overlay_rgb = draw_grid_and_text_on_sum_image(sum16, circle_params, sum_counts)

            prog.set_progress(len(tif_files) + 2, "保存叠加图（raw16 + overlay_vis，覆盖同名文件）...")
            raw16_path, vis_path = save_folder_pixel_sum_double_outputs(folder, sum16, overlay_rgb)
        except Exception as e:
            fail += 1
            errors.append(f"生成/保存叠加图失败: {e}")

        try:
            prog.set_progress(total_steps, "完成")
            prog.destroy()
        except Exception:
            pass

        msg = [
            "批量检测完成。",
            f"文件夹：{folder}",
            f"共处理：{len(tif_files)}",
            f"��功：{ok}",
            f"失败：{fail}",
            "",
            "输出文件：",
            "1) 每张图片同名 .txt（覆盖同名文件）",
            "2) folder_big_spot_summary.txt（覆盖同名文件）",
            "3) folder_pixel_sum_raw16.tif（叠加饱和到65535，覆盖同名文件）",
            "4) folder_pixel_sum_overlay_vis.tif（可视化：网格+文字，覆盖同名文件）",
        ]
        if summary_txt_path:
            msg.append(f"汇总TXT：{summary_txt_path}")
        if raw16_path:
            msg.append(f"raw16：{raw16_path}")
        if vis_path:
            msg.append(f"vis：{vis_path}")
        if errors:
            msg.append("")
            msg.append("失败/警告（前20条）：")
            msg.extend(errors[:20])

        messagebox.showinfo("批量检测结果", "\n".join(msg))


if __name__ == "__main__":
    app = MultiTifViewer()
    app.mainloop()
