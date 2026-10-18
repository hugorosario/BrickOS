const features = [...document.querySelectorAll('.feature')];
const featureImage = document.querySelector('#feature-image');
const featureLink = document.querySelector('#feature-capture');
const featureCaption = document.querySelector('#feature-caption');
for (const feature of features) {
  feature.addEventListener('toggle', () => {
    if (!feature.open) return;
    for (const other of features) if (other !== feature) other.open = false;
    const path = `screenshots/${feature.dataset.image}`;
    featureImage.src = path;
    featureImage.alt = feature.dataset.alt;
    featureLink.href = path;
    featureLink.dataset.caption = feature.dataset.caption;
    featureCaption.textContent = feature.dataset.caption;
  });
}

const dialog = document.querySelector('#capture-dialog');
const dialogImage = document.querySelector('#dialog-image');
const dialogCaption = document.querySelector('#dialog-caption');
if (typeof dialog.showModal === 'function') {
  for (const link of document.querySelectorAll('.capture-link')) {
    link.addEventListener('click', event => {
      if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      dialogImage.src = link.href;
      dialogImage.alt = link.querySelector('img').alt;
      dialogCaption.textContent = link.dataset.caption;
      dialog.showModal();
      document.body.classList.add('dialog-open');
    });
  }
  document.querySelector('#close-capture').addEventListener('click', () => dialog.close());
  dialog.addEventListener('click', event => { if (event.target === dialog) dialog.close(); });
  dialog.addEventListener('close', () => document.body.classList.remove('dialog-open'));
}
