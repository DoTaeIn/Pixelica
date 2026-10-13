// 웹 학습용 Canvas 데모. Pixelica C 엔진을 실행하지 않습니다.
const canvas = document.querySelector('#screen');
const ctx = canvas.getContext('2d');
const color = '#a9f478'; // 색을 바꿔 보세요.
const speed = 0.001;
function draw(time) {
  ctx.fillStyle = '#111813';
  ctx.fillRect(0, 0, 64, 64);
  ctx.fillStyle = color;
  for (let y = 0; y < 8; y++) {
    for (let x = 0; x < 8; x++) {
      const size = 2 + Math.sin(time * speed + x * 0.7 + y * 0.5) * 2;
      ctx.fillRect(x * 8 + 4 - size / 2, y * 8 + 4 - size / 2, size, size);
    }
  }
  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
