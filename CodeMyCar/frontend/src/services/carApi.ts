import type { MotionPayload } from '@/lib/control'

export interface DeviceStatus {
  device_id: string
  online: boolean
  last_heartbeat_age: number | null
  uptime_s: number | null
  heap_free: number | null
  stm32_connected: boolean | null
  front_distance_cm: number | null
  rear_distance_cm: number | null
  sensor_age_s: number | null
}

export interface MotionState {
  ok: boolean
  online: boolean
  stm32_connected: boolean
  motion_supported: boolean
  motion: {
    state: string
    seq: number | null
    age_ms: number | null
    stale: boolean
  }
  pose: {
    x_cm: number
    y_cm: number
    heading_deg: number
    odometer_cm: number
  }
  velocity: {
    linear_cm_s: number
    angular_deg_s: number
  }
}

export interface MoveRequest {
  distance_cm: number
  turn_deg: number
  speed_cm_s: number
}

interface CommonResponse<T> {
  success: boolean
  message: string
  data: T
}

export interface ApiConfig {
  baseUrl: string
  apiKey: string
}

function normalizeBaseUrl(value: string): string {
  return value.trim().replace(/\/+$/, '')
}

export function defaultApiBase(): string {
  const host = window.location.hostname || '127.0.0.1'
  return `${window.location.protocol}//${host}:8000`
}

export class CarApi {
  private config: ApiConfig

  constructor(config: ApiConfig) {
    this.config = { ...config, baseUrl: normalizeBaseUrl(config.baseUrl) }
  }

  updateConfig(config: ApiConfig): void {
    this.config = { ...config, baseUrl: normalizeBaseUrl(config.baseUrl) }
  }

  private async request<T>(path: string, init: RequestInit = {}): Promise<T> {
    const headers = new Headers(init.headers)
    headers.set('Accept', 'application/json')
    if (init.body) headers.set('Content-Type', 'application/json')
    if (this.config.apiKey) headers.set('X-API-Key', this.config.apiKey)

    const response = await fetch(`${this.config.baseUrl}${path}`, { ...init, headers })
    const body = await response.json().catch(() => null)
    if (!response.ok) {
      const detail = body?.detail
      const message = detail?.message || detail?.code || detail || `HTTP ${response.status}`
      throw new Error(typeof message === 'string' ? message : JSON.stringify(message))
    }
    return body as T
  }

  async listDevices(): Promise<DeviceStatus[]> {
    const result = await this.request<CommonResponse<DeviceStatus[]>>('/api/v1/devices')
    return result.data ?? []
  }

  async getDeviceStatus(deviceId: string): Promise<DeviceStatus> {
    const result = await this.request<CommonResponse<DeviceStatus>>(
      `/api/v1/devices/${encodeURIComponent(deviceId)}/status`,
    )
    return result.data
  }

  getMotionState(deviceId: string): Promise<MotionState> {
    return this.request(`/api/v1/agent/devices/${encodeURIComponent(deviceId)}/state`)
  }

  async sendControl(deviceId: string, payload: MotionPayload): Promise<void> {
    const result = await this.request<CommonResponse<null>>(
      `/api/v1/devices/${encodeURIComponent(deviceId)}/commands`,
      { method: 'POST', body: JSON.stringify(payload) },
    )
    if (!result.success) throw new Error(result.message || '命令发送失败')
  }

  async stop(deviceId: string): Promise<void> {
    await this.request(`/api/v1/agent/devices/${encodeURIComponent(deviceId)}/stop`, {
      method: 'POST',
    })
  }

  move(deviceId: string, request: MoveRequest): Promise<unknown> {
    return this.request(`/api/v1/agent/devices/${encodeURIComponent(deviceId)}/move`, {
      method: 'POST',
      body: JSON.stringify(request),
    })
  }

  async ping(deviceId: string): Promise<number | null> {
    const started = performance.now()
    const result = await this.request<CommonResponse<unknown>>(
      `/api/v1/devices/${encodeURIComponent(deviceId)}/ping`,
      { method: 'POST' },
    )
    if (!result.success) throw new Error(result.message || 'Ping 失败')
    return Math.round(performance.now() - started)
  }
}
