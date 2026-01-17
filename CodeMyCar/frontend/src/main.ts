import { createApp } from 'vue'
import { createPinia } from 'pinia'
import { Quasar } from 'quasar'
import quasarIconSet from 'quasar/icon-set/material-icons'
import 'quasar/src/css/index.sass'
// 导入 Quasar 组件
import { QBtnToggle, QSlider, QBtn, QLayout, QPageContainer, QPage } from 'quasar'

import App from './App.vue'
import router from './router'

const app = createApp(App)

app.use(createPinia())
app.use(router)

app.use(Quasar, {
  plugins: {},
  iconSet: quasarIconSet,
  components: {
    QBtnToggle,
    QSlider,
    QBtn,
    QLayout,
    QPageContainer,
    QPage,
  },
})

app.mount('#app')
