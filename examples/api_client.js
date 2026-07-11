/**
 * Minimal JSON API client with retry and caching.
 * Demo file for Code Annotator.
 */

const BASE_URL = "https://api.example.com/v1";
const MAX_RETRIES = 3;

class ApiClient {
  constructor(token) {
    this.token = token;
    this.cache = new Map();
  }

  headers() {
    return {
      "Authorization": `Bearer ${this.token}`,
      "Content-Type": "application/json",
    };
  }

  async get(path) {
    // serve repeated reads from the in-memory cache
    if (this.cache.has(path)) {
      return this.cache.get(path);
    }
    const data = await this.request("GET", path);
    this.cache.set(path, data);
    return data;
  }

  async request(method, path, body = null) {
    let lastError;
    for (let attempt = 1; attempt <= MAX_RETRIES; attempt++) {
      try {
        const res = await fetch(`${BASE_URL}${path}`, {
          method,
          headers: this.headers(),
          body: body ? JSON.stringify(body) : undefined,
        });
        if (res.status === 429) {
          // backoff grows linearly with the attempt number
          await sleep(500 * attempt);
          continue;
        }
        if (!res.ok) {
          throw new Error(`${method} ${path} -> ${res.status}`);
        }
        return await res.json();
      } catch (err) {
        lastError = err;
      }
    }
    throw lastError;
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

module.exports = { ApiClient };
