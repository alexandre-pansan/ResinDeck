// 3D preview built from the voxel grid the server derives from the layer images.
import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

export class Viewer3D {
  constructor(canvas) {
    this.canvas = canvas;
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(window.devicePixelRatio);
    this.renderer.localClippingEnabled = true;
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0x0b0d10);
    this.camera = new THREE.PerspectiveCamera(40, 1, 0.5, 2000);
    this.controls = new OrbitControls(this.camera, canvas);
    this.controls.enableDamping = true;

    this.scene.add(new THREE.HemisphereLight(0xdde6ff, 0x202530, 1.6));
    const sun = new THREE.DirectionalLight(0xffffff, 1.6);
    sun.position.set(80, 160, 120);
    this.scene.add(sun);
    const fill = new THREE.DirectionalLight(0x88aaff, 0.5);
    fill.position.set(-120, 40, -80);
    this.scene.add(fill);

    // y up: keep everything below the cut, ghost everything above it
    this.cutBelow = new THREE.Plane(new THREE.Vector3(0, -1, 0), 1e6);
    this.cutAbove = new THREE.Plane(new THREE.Vector3(0, 1, 0), -1e6);
    this.solidMat = new THREE.MeshStandardMaterial({
      color: 0x4f9dff, roughness: 0.55, metalness: 0.05, flatShading: true, side: THREE.DoubleSide,
      clippingPlanes: [this.cutBelow],
    });
    this.ghostMat = new THREE.MeshStandardMaterial({
      color: 0x9fb4d0, roughness: 0.8, flatShading: true, transparent: true, opacity: 0.12,
      depthWrite: false, clippingPlanes: [this.cutAbove],
    });
    this.cutRing = null;
    this.group = new THREE.Group();
    this.scene.add(this.group);

    new ResizeObserver(() => this.resize()).observe(canvas.parentElement);
    this.resize();
    const tick = () => {
      this.controls.update();
      this.renderer.render(this.scene, this.camera);
      requestAnimationFrame(tick);
    };
    tick();
  }

  resize() {
    const el = this.canvas.parentElement;
    const w = el.clientWidth, h = el.clientHeight;
    if (!w || !h) return;
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  clear() {
    for (const obj of [...this.group.children]) {
      obj.geometry?.dispose();
      this.group.remove(obj);
    }
  }

  // payload: uint32 meta length, JSON meta, np.packbits(grid) with grid shaped (z, y, x)
  load(buffer) {
    this.clear();
    const metaLen = new DataView(buffer).getUint32(0, true);
    const meta = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, 4, metaLen)));
    const bits = new Uint8Array(buffer, 4 + metaLen);
    this.meta = meta;
    this.addBed(meta.bed);
    if (!meta.shape) return;

    const geometry = buildSurface(bits, meta);
    this.group.add(new THREE.Mesh(geometry, this.solidMat));
    this.group.add(new THREE.Mesh(geometry, this.ghostMat));

    const [ox, oy, oz] = meta.origin;
    const [vx, vy, vz] = meta.voxel;
    const [nz, ny, nx] = meta.shape;
    this.top = oz + nz * vz;
    const center = new THREE.Vector3(ox + nx * vx / 2, this.top / 2, oy + ny * vy / 2);
    const size = Math.max(nx * vx, ny * vy, this.top);
    this.controls.target.copy(center);
    this.camera.position.copy(center).add(new THREE.Vector3(size * 1.3, size * 0.8, size * 1.6));
    this.camera.near = size / 100;
    this.camera.far = size * 50;
    this.camera.updateProjectionMatrix();
    this.setCut(null);
  }

  addBed([bx, by, bz]) {
    const plate = new THREE.Mesh(
      new THREE.PlaneGeometry(bx, by),
      new THREE.MeshBasicMaterial({ color: 0x1a2028, side: THREE.DoubleSide }),
    );
    plate.rotation.x = -Math.PI / 2;
    plate.position.set(bx / 2, -0.05, by / 2);
    this.group.add(plate);
    const grid = new THREE.GridHelper(Math.max(bx, by), Math.round(Math.max(bx, by) / 10), 0x2c3540, 0x222a33);
    grid.scale.set(bx / Math.max(bx, by), 1, by / Math.max(bx, by));
    grid.position.set(bx / 2, 0, by / 2);
    this.group.add(grid);
    const box = new THREE.LineSegments(
      new THREE.EdgesGeometry(new THREE.BoxGeometry(bx, bz, by)),
      new THREE.LineBasicMaterial({ color: 0x2c3540 }),
    );
    box.position.set(bx / 2, bz / 2, by / 2);
    this.group.add(box);
    this.bed = [bx, by, bz];
  }

  // height in mm; null shows the whole model solid
  setCut(height) {
    const h = height == null ? 1e6 : height;
    this.cutBelow.constant = h;
    this.cutAbove.constant = -h;
    this.ghostMat.visible = height != null;
    if (this.cutRing) {
      this.group.remove(this.cutRing);
      this.cutRing.geometry.dispose();
      this.cutRing = null;
    }
    if (height != null && this.bed) {
      const [bx, by] = this.bed;
      const pts = [[0, 0], [bx, 0], [bx, by], [0, by]].map(([x, z]) => new THREE.Vector3(x, h, z));
      this.cutRing = new THREE.LineLoop(
        new THREE.BufferGeometry().setFromPoints(pts),
        new THREE.LineBasicMaterial({ color: 0x3b8cff }),
      );
      this.group.add(this.cutRing);
    }
  }
}

// One quad per exposed voxel face. World axes: x = image x, y = height, z = image row.
function buildSurface(bits, meta) {
  const [nz, ny, nx] = meta.shape;
  const [vx, vy, vz] = meta.voxel;
  const [ox, oy, oz] = meta.origin;
  const n = nx * ny * nz;
  const vox = new Uint8Array(n);
  for (let i = 0; i < n; i++) vox[i] = (bits[i >> 3] >> (7 - (i & 7))) & 1;
  const at = (z, y, x) =>
    x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz ? vox[(z * ny + y) * nx + x] : 0;

  // [dz, dy, dx] neighbour offset and the 4 corners of that face (in voxel units, z/y/x)
  const faces = [
    [[1, 0, 0], [[1, 0, 0], [1, 0, 1], [1, 1, 1], [1, 1, 0]]],
    [[-1, 0, 0], [[0, 0, 0], [0, 1, 0], [0, 1, 1], [0, 0, 1]]],
    [[0, 1, 0], [[0, 1, 0], [1, 1, 0], [1, 1, 1], [0, 1, 1]]],
    [[0, -1, 0], [[0, 0, 0], [0, 0, 1], [1, 0, 1], [1, 0, 0]]],
    [[0, 0, 1], [[0, 0, 1], [0, 1, 1], [1, 1, 1], [1, 0, 1]]],
    [[0, 0, -1], [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]],
  ];

  let count = 0;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        if (!vox[(z * ny + y) * nx + x]) continue;
        for (const [[dz, dy, dx]] of faces) if (!at(z + dz, y + dy, x + dx)) count++;
      }

  const pos = new Float32Array(count * 12);
  const idx = count * 4 > 65535 ? new Uint32Array(count * 6) : new Uint16Array(count * 6);
  let f = 0;
  for (let z = 0; z < nz; z++)
    for (let y = 0; y < ny; y++)
      for (let x = 0; x < nx; x++) {
        if (!vox[(z * ny + y) * nx + x]) continue;
        for (const [[dz, dy, dx], corners] of faces) {
          if (at(z + dz, y + dy, x + dx)) continue;
          const p = f * 12;
          for (let c = 0; c < 4; c++) {
            const [cz, cy, cx] = corners[c];
            pos[p + c * 3] = ox + (x + cx) * vx;
            pos[p + c * 3 + 1] = oz + (z + cz) * vz;
            pos[p + c * 3 + 2] = oy + (y + cy) * vy;
          }
          const v = f * 4, q = f * 6;
          idx[q] = v; idx[q + 1] = v + 1; idx[q + 2] = v + 2;
          idx[q + 3] = v; idx[q + 4] = v + 2; idx[q + 5] = v + 3;
          f++;
        }
      }

  const g = new THREE.BufferGeometry();
  g.setAttribute("position", new THREE.BufferAttribute(pos, 3));
  g.setIndex(new THREE.BufferAttribute(idx, 1));
  g.computeBoundingSphere();
  return g;
}
