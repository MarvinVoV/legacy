export type DriveMode = 'drive' | 'direct'
export type DriveProfile = 'precision' | 'standard' | 'sport'

export interface ControlVector {
  throttle: number
  steering: number
}

export interface MotionPayload {
  seq: number
  cmd: 'motion'
  motion: {
    mode: 0 | 1 | 3
    params: Record<string, number>
  }
}

export const profiles = {
  precision: { label: '精细', maxMmS: 250, maxRpm: 75, accent: '#7cf7c9' },
  standard: { label: '标准', maxMmS: 500, maxRpm: 150, accent: '#69e7ff' },
  sport: { label: '运动', maxMmS: 800, maxRpm: 240, accent: '#ffb86b' },
} as const

export function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value))
}

export function applyDeadzone(value: number, deadzone = 0.06): number {
  const magnitude = Math.abs(value)
  if (magnitude <= deadzone) return 0
  return Math.sign(value) * ((magnitude - deadzone) / (1 - deadzone))
}

export function pointToControl(
  clientX: number,
  clientY: number,
  rect: Pick<DOMRect, 'left' | 'top' | 'width' | 'height'>,
): ControlVector {
  const x = clamp(((clientX - rect.left) / rect.width) * 2 - 1, -1, 1)
  const y = clamp(1 - ((clientY - rect.top) / rect.height) * 2, -1, 1)
  return {
    steering: Math.round(applyDeadzone(x) * 100),
    throttle: Math.round(applyDeadzone(y) * 100),
  }
}

export function buildControlPayload(
  seq: number,
  mode: DriveMode,
  profile: DriveProfile,
  vector: ControlVector,
): MotionPayload {
  const safeSeq = ((seq % 256) + 256) % 256
  const throttle = clamp(Math.round(vector.throttle), -100, 100)
  const steering = clamp(Math.round(vector.steering), -100, 100)
  const curvature = Math.sign(steering) * Math.round((Math.abs(steering) / 100) * 3770) / 1000
  const limits = profiles[profile]

  if (mode === 'direct') {
    const rpm = Math.round((throttle / 100) * limits.maxRpm)
    return {
      seq: safeSeq,
      cmd: 'motion',
      motion: {
        mode: 1,
        params: { left_rpm: rpm, right_rpm: rpm, curvature_1pm: curvature },
      },
    }
  }

  return {
    seq: safeSeq,
    cmd: 'motion',
    motion: {
      mode: 3,
      params: {
        linear_vel: Math.round((throttle / 100) * limits.maxMmS),
        curvature_1pm: curvature,
      },
    },
  }
}

export function buildEmergencyStopPayload(seq: number): MotionPayload {
  return {
    seq: ((seq % 256) + 256) % 256,
    cmd: 'motion',
    motion: { mode: 0, params: {} },
  }
}
