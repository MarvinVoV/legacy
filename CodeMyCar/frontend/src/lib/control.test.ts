import { describe, expect, it } from 'vitest'
import { buildControlPayload, buildEmergencyStopPayload, pointToControl } from './control'

describe('control mapping', () => {
  it('maps the drive pad corners to signed percentages', () => {
    const rect = { left: 10, top: 20, width: 200, height: 100 }
    expect(pointToControl(210, 20, rect)).toEqual({ throttle: 100, steering: 100 })
    expect(pointToControl(10, 120, rect)).toEqual({ throttle: -100, steering: -100 })
  })

  it('creates DRIVE commands using the selected speed profile', () => {
    expect(buildControlPayload(257, 'drive', 'standard', { throttle: 50, steering: -25 })).toEqual({
      seq: 1,
      cmd: 'motion',
      motion: { mode: 3, params: { linear_vel: 250, curvature_1pm: -0.943 } },
    })
  })

  it('creates DIRECT commands with bounded RPM and steering', () => {
    expect(buildControlPayload(9, 'direct', 'sport', { throttle: 200, steering: -130 })).toEqual({
      seq: 9,
      cmd: 'motion',
      motion: { mode: 1, params: { left_rpm: 240, right_rpm: 240, curvature_1pm: -3.77 } },
    })
  })

  it('creates a zero-parameter emergency stop', () => {
    expect(buildEmergencyStopPayload(256).motion).toEqual({ mode: 0, params: {} })
  })
})
