import { test, expect } from '@playwright/test'

test('renders the mobile driving console', async ({ page }) => {
  await page.goto('/?mock=1')
  await expect(page.locator('h1')).toHaveText('Drive Console')
  await expect(page.getByRole('button', { name: /STOP/ })).toBeVisible()
  await expect(page.getByRole('slider', { name: '速度与转向控制摇杆' })).toBeVisible()
})
