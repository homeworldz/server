export const API_BASE_URL =
  import.meta.env.VITE_API_BASE_URL ?? "https://api.homeworldz.com/v1";

// The grid service's own public base, used for the files it serves directly
// rather than through the website API - today, the daily statistics CSV.
export const GRID_BASE_URL =
  import.meta.env.VITE_GRID_BASE_URL ?? "https://grid.homeworldz.com";
