#!/usr/bin/env python3
"""Petri's native desktop observatory."""

import argparse
from dataclasses import replace
import math
from pathlib import Path
from queue import SimpleQueue
from struct import iter_unpack
from sys import byteorder
from threading import Event, Thread

import cairo
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk

from transport import Camera, PANEL_HEIGHT, PANEL_WIDTH, SPRITE_ZOOM, View, demo_observations, usb_observations


ROOT = Path(__file__).resolve().parents[1]
CSS = b"""
window { background: #121919; color: #e0eae4; font-family: Inter, sans-serif; }
headerbar { background: #182222; border-bottom: 1px solid #344340; }
.view-toolbar { padding: 9px 14px; background: #1b2625; }
.metrics { color: #b8cbc1; font-family: "IBM Plex Mono", monospace; }
.panel { background: #182221; border-left: 1px solid #344340; padding: 19px; }
.heading { font-size: 19px; font-weight: 650; }
.muted { color: #9aafa6; }
.value { font-family: "IBM Plex Mono", monospace; color: #e2ece5; }
.legend { padding: 7px 14px 9px; color: #acc2b7; }
.terrain { background: #101716; }
button, entry { border-radius: 4px; }
button.suggested-action { background: #b6e0c6; color: #172c21; }
"""


def text(value="", *classes):
    label = Gtk.Label(label=value, xalign=0)
    for item in classes:
        label.add_css_class(item)
    return label


def frame_surface(pixels):
    data = bytearray()
    for (color,) in iter_unpack("<H", pixels):
        # The panel takes the high four bits of each RGB565 channel.
        r = (color >> 12) * 17
        g = (color >> 7 & 15) * 17
        b = (color >> 1 & 15) * 17
        data.extend((0, r, g, b) if byteorder == "big" else (b, g, r, 0))
    return cairo.ImageSurface.create_for_data(
        data, cairo.FORMAT_RGB24, PANEL_WIDTH, PANEL_HEIGHT, PANEL_WIDTH * 4)


class Mirror(Gtk.DrawingArea):
    def __init__(self, update_inspector):
        super().__init__(hexpand=True, vexpand=True)
        self.add_css_class("terrain")
        self.set_content_width(520)
        self.set_content_height(340)
        self.set_draw_func(self.draw)
        self.update_inspector = update_inspector
        self.clear()

        click = Gtk.GestureClick()
        click.set_button(1)
        click.connect("released", self.click)
        self.add_controller(click)
        drag = Gtk.GestureDrag()
        drag.set_button(1)
        drag.connect("drag-begin", self.drag_begin)
        drag.connect("drag-update", self.drag_update)
        self.add_controller(drag)
        drag.group(click)
        wheel = Gtk.EventControllerScroll.new(
            Gtk.EventControllerScrollFlags.VERTICAL | Gtk.EventControllerScrollFlags.DISCRETE)
        wheel.connect("scroll", self.scroll)
        self.add_controller(wheel)
        self.pointer = (None, None)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self.record_pointer)
        motion.connect("leave", lambda *_: self.record_pointer(None, None, None))
        self.add_controller(motion)

    def clear(self):
        self.world = self.surface = self.selected = None
        self.control = None
        self.pending_camera = None
        self.revision = 0
        self.drag_origin = None
        self.dragged = False
        self.queue_draw()

    @property
    def camera(self):
        return self.pending_camera or (self.world.camera if self.world else Camera(View.LIFE, 0))

    def set_world(self, world):
        if self.world and world.tick < self.world.tick:
            self.selected = None
            self.pending_camera = None
        elif (self.world and self.selected == self.world.camera.follow and self.selected not in world.creatures
              and world.camera.follow in world.creatures):
            self.selected = world.camera.follow
        self.world = world
        if self.pending_camera is None or world.revision == self.revision:
            self.pending_camera = None
            self.revision = world.revision
        self.surface = frame_surface(world.pixels)
        self.queue_draw()

    def send(self, camera):
        if self.control is None or self.world is None or camera == self.camera:
            return
        self.revision = (self.revision + 1) & 0xffffffff
        self.pending_camera = camera
        self.control(camera.command(self.revision))
        self.update_inspector(self.selected)

    def scale(self):
        return max(0.1, min((self.get_width() - 32) / PANEL_WIDTH,
                            (self.get_height() - 32) / PANEL_HEIGHT))

    def panel_coords(self, x, y):
        scale = self.scale()
        px = PANEL_WIDTH / 2 if x is None else (x - self.get_width() / 2) / scale + PANEL_WIDTH / 2
        py = PANEL_HEIGHT / 2 if y is None else (y - self.get_height() / 2) / scale + PANEL_HEIGHT / 2
        return px, py

    def clamp(self, camera):
        if not camera.zoom:
            return replace(camera, left=0, top=0)
        tile = 1 << (camera.zoom - 1)
        extra = tile // 2 if self.world.staggered_rows and tile >= 4 else 0
        return replace(camera, left=max(0, min(camera.left, self.world.width * tile + extra - PANEL_WIDTH)),
                       top=max(0, min(camera.top, self.world.height * tile - PANEL_HEIGHT)))

    def zoom_to(self, zoom, x=None, y=None, follow=None):
        if not self.world:
            return
        camera = self.camera
        zoom = max(0, min(self.world.max_zoom, zoom))
        view = View.CLOSE if zoom >= SPRITE_ZOOM else View.LIFE
        if camera.view == View.HABITAT and zoom < SPRITE_ZOOM and not follow:
            view = View.HABITAT
        if follow is None:
            follow = camera.follow
        old_tile = 2 ** (camera.zoom - 1) if camera.zoom else PANEL_WIDTH / self.world.width
        new_tile = 2 ** (zoom - 1) if zoom else PANEL_WIDTH / self.world.width
        px, py = self.panel_coords(x, y)
        left = round((camera.left + px) * new_tile / old_tile - px)
        if self.world.staggered_rows and int((camera.top + py) / old_tile) % 2:
            left += round(new_tile / 2 * ((zoom >= 3) - (camera.zoom >= 3)))
        top = round((camera.top + py) * new_tile / old_tile - py)
        self.send(self.clamp(Camera(view, zoom, left, top, follow)))

    def zoom_by(self, direction, x=None, y=None):
        if not self.world:
            return
        zoom = max(0, min(self.world.max_zoom, self.camera.zoom + direction))
        if zoom != self.camera.zoom:
            self.zoom_to(zoom, x, y)

    def change_view(self, view):
        if view == View.CLOSE:
            self.zoom_to(SPRITE_ZOOM)
        else:
            self.send(Camera(view, 0))

    def record_pointer(self, _controller, x, y):
        self.pointer = (x, y)

    def scroll(self, _controller, _dx, dy):
        if dy:
            self.zoom_by(1 if dy < 0 else -1, *self.pointer)
        return True

    def drag_begin(self, _gesture, _x, _y):
        self.drag_origin = self.camera
        self.dragged = False

    def drag_update(self, _gesture, dx, dy):
        camera = self.drag_origin
        if self.world and camera and camera.zoom and math.hypot(dx, dy) > 4:
            self.dragged = True
            scale = self.scale()
            self.send(self.clamp(replace(camera, left=camera.left - round(dx / scale),
                                         top=camera.top - round(dy / scale), follow=0)))

    def click(self, _gesture, presses, x, y):
        if not self.world or not presses or self.dragged:
            return
        px, py = self.panel_coords(x, y)
        if not (0 <= px < PANEL_WIDTH and 0 <= py < PANEL_HEIGHT):
            return
        camera = self.world.camera
        radius = 5 * 2 ** (camera.zoom - SPRITE_ZOOM) if camera.view == View.CLOSE else 1.5
        center_offset = 0 if camera.zoom == SPRITE_ZOOM - 1 else 0.5
        closest = None
        for creature in self.world.creatures.values():
            if creature.x < 0:
                continue
            distance = math.hypot(px - creature.x - center_offset, py - creature.y - center_offset)
            if distance <= radius:
                closest, radius = creature.id, distance
        self.selected = closest
        self.update_inspector(closest)

    def draw(self, _area, cr, width, height):
        cr.set_source_rgb(0.043, 0.063, 0.059)
        cr.paint()
        if self.surface:
            scale = self.scale()
            cr.translate((width - PANEL_WIDTH * scale) / 2, (height - PANEL_HEIGHT * scale) / 2)
            cr.scale(scale, scale)
            pattern = cairo.SurfacePattern(self.surface)
            pattern.set_filter(cairo.FILTER_NEAREST)
            cr.set_source(pattern)
            cr.rectangle(0, 0, PANEL_WIDTH, PANEL_HEIGHT)
            cr.fill()


class Observatory(Gtk.Application):
    def __init__(self, args):
        super().__init__(application_id="org.petri.Observatory", flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.args = args
        self.stop = Event()
        self.token = 0

    def do_activate(self):
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.window = Gtk.ApplicationWindow(application=self, default_width=1100, default_height=680)
        header = Gtk.HeaderBar()
        header.set_title_widget(text("Petri", "heading"))
        self.window.set_titlebar(header)

        demo = Gtk.Button(label="Demo")
        demo.connect("clicked", lambda *_: self.open_source(None))
        header.pack_start(demo)
        connect = Gtk.Button(label="Connect USB")
        connect.add_css_class("suggested-action")
        connect.connect("clicked", lambda *_: self.open_source(self.device.get_text().strip()))
        header.pack_end(connect)
        self.device = Gtk.Entry(text=self.args.device or "/dev/ttyACM0", width_chars=16)
        self.device.set_tooltip_text("MatrixPortal serial device")
        self.device.connect("activate", lambda entry: self.open_source(entry.get_text().strip()))
        header.pack_end(self.device)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.window.set_child(root)
        toolbar = Gtk.Box(spacing=6)
        toolbar.add_css_class("view-toolbar")
        root.append(toolbar)
        self.source = text("", "muted")
        self.source.set_hexpand(True)
        toolbar.append(self.source)
        self.metrics = text("", "metrics")
        toolbar.append(self.metrics)
        toolbar.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))

        self.map = Mirror(self.update_inspector)
        self.views = Gtk.DropDown.new_from_strings(["Sprites", "Hunt", "Life", "Habitat"])
        self.view_signal = self.views.connect("notify::selected", self.change_view)
        self.view_controls = [self.views]
        toolbar.append(self.views)
        for caption, action in (("−", lambda: self.map.zoom_by(-1)),
                                ("+", lambda: self.map.zoom_by(1)), ("Fit", self.reset_view)):
            button = Gtk.Button(label=caption)
            button.connect("clicked", lambda _, fn=action: fn())
            self.view_controls.append(button)
            toolbar.append(button)

        columns = Gtk.Box(vexpand=True)
        root.append(columns)
        columns.append(self.map)
        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=13, width_request=252)
        panel.add_css_class("panel")
        columns.append(panel)
        self.inspector = text("Creature", "heading")
        panel.append(self.inspector)
        self.detail = text("Select a creature on the panel.", "muted")
        self.detail.set_wrap(True)
        panel.append(self.detail)
        self.follow = Gtk.Button(label="Follow")
        self.follow.set_sensitive(False)
        self.follow.connect("clicked", self.toggle_follow)
        panel.append(self.follow)
        self.grid = Gtk.Grid(column_spacing=22, row_spacing=9)
        panel.append(self.grid)
        self.values = []
        for i, name in enumerate(("Position", "Energy", "Generation", "Parent", "Founder",
                                 "Diet tendency", "Plant digestion", "Meat digestion", "Thrift",
                                 "Faction", "Moisture preference", "Plant food eaten", "Animal food eaten")):
            label = text(name, "muted")
            value = text("—", "value")
            self.grid.attach(label, 0, i, 1, 1)
            self.grid.attach(value, 1, i, 1, 1)
            self.values.append(value)
        self.grid.set_visible(False)

        self.window.connect("close-request", self.close)
        self.window.present()
        self.open_source(self.args.device)

    def close(self, _window):
        self.stop.set()
        return False

    def open_source(self, device):
        self.stop.set()
        self.stop = Event()
        self.token += 1
        token, stopper = self.token, self.stop
        self.source.set_label(f"Connecting to {device}…" if device else "Starting demo…")
        self.metrics.set_label("")
        self.map.clear()
        self.enable_camera(False)
        commands = SimpleQueue()
        self.map.control = commands.put
        self.update_inspector(None)

        def receive():
            try:
                frames = usb_observations(device, commands, stopper) if device else demo_observations(
                    self.args.binary, self.args.seed, commands, stopper)
                for world in frames:
                    GLib.idle_add(self.new_world, token, device, world)
            except (OSError, ValueError) as error:
                message = str(error)
                if isinstance(error, PermissionError) and device:
                    message = f"USB access denied for {device}"
                GLib.idle_add(self.source_error, token, message)

        Thread(target=receive, daemon=True).start()

    def source_error(self, token, message):
        if token == self.token:
            self.source.set_label(message)
            self.map.control = None
            self.map.pending_camera = None
            self.enable_camera(False)
            self.update_inspector(self.map.selected)
        return False

    def new_world(self, token, device, world):
        if token == self.token and not self.stop.is_set():
            self.map.set_world(world)
            self.enable_camera(True)
            self.views.handler_block(self.view_signal)
            self.views.set_selected(int(self.map.camera.view))
            self.views.handler_unblock(self.view_signal)
            self.source.set_label(f"USB · {device}" if device else f"Demo · seed {self.args.seed}")
            self.metrics.set_label(f"Tick {world.tick:,}   {len(world.creatures)} creatures   {world.kills:,} kills")
            self.update_inspector(self.map.selected)
        return False

    def update_inspector(self, selected):
        world = self.map.world
        creature = world.creatures.get(selected) if world and selected else None
        following = creature is not None and selected == self.map.camera.follow
        self.follow.set_label("Stop following" if following else "Follow")
        self.follow.set_sensitive(creature is not None and self.map.control is not None)
        self.grid.set_visible(creature is not None)
        if not creature:
            self.inspector.set_label(f"Creature {selected}" if selected else "Creature")
            self.detail.set_label("No longer alive in this snapshot." if selected else "Select a creature on the panel.")
            return
        self.inspector.set_label(f"Creature {creature.id}")
        self.detail.set_label("Following" if following else "Selected")
        diet = "Omnivore"
        if creature.digestion >= 10:
            diet = "Herbivore"
        elif creature.digestion <= 6:
            diet = "Carnivore"
        pairs = (
            f"{creature.cell % world.width}, {creature.cell // world.width}",
            f"{creature.energy} / 160",
            creature.generation, creature.parent or "—", creature.founder,
            diet, f"{creature.digestion} / 15", f"{16 - creature.digestion} / 15",
            f"{creature.thrift} / 15", creature.faction or "—",
            str(12 + creature.moisture_preference * 12) if creature.moisture_preference else "—",
            f"{creature.plants_eaten:,}", f"{creature.animal_eaten:,}",
        )
        for label, value in zip(self.values, pairs):
            label.set_label(str(value))

    def change_view(self, dropdown, _property):
        self.map.change_view(View(dropdown.get_selected()))

    def enable_camera(self, enabled):
        for control in self.view_controls:
            control.set_sensitive(enabled)

    def reset_view(self):
        self.map.send(Camera(View.LIFE, 0))

    def toggle_follow(self, _button):
        camera = self.map.camera
        selected = self.map.selected
        if selected is None:
            return
        if camera.follow == selected:
            self.map.send(replace(camera, follow=0))
        else:
            zoom = max(camera.zoom, 2)
            self.map.zoom_to(zoom, follow=selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="start with a MatrixPortal USB serial device")
    parser.add_argument("--binary", type=Path, default=ROOT / "build-wide/petri_observer")
    parser.add_argument("--seed", type=int, default=14)
    args = parser.parse_args()
    if not Gtk.init_check():
        parser.exit(1, "Petri needs a running GTK desktop display.\n")
    return Observatory(args).run(None)


if __name__ == "__main__":
    raise SystemExit(main())
