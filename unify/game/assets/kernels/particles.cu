// Sparkle particles: semi-implicit Euler with gravity, one thread per particle.
__global__ void particles(float* px, float* py, float* vx, float* vy, float* life, int n, float dt, float gravity) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (life[i] <= 0.0f) return;
    vy[i] = vy[i] + gravity * dt;
    vx[i] = vx[i] * 0.98f;
    px[i] = px[i] + vx[i] * dt;
    py[i] = py[i] + vy[i] * dt;
    life[i] = life[i] - dt;
}
